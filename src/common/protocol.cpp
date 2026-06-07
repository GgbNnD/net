// ============================================================
// 应用层协议实现
// ============================================================

#include "common/protocol.h"
#include "common/platform.h"
#include "common/utils.h"
#include "common/peer_key.h"
#include <cstring>
#include <vector>
#include <cstdint>

namespace Protocol {

constexpr const char* MAC_SECRET = "p2p-transfer-secret-2024";

static std::string mac_secret_for(const std::string& peer_ip) {
    std::string peer = PeerKey::get(peer_ip);
    if (peer.empty()) return MAC_SECRET;
    return std::string(MAC_SECRET) + peer;
}

// ============================================================
// 协议消息构建函数
// ============================================================

json build_device_broadcast(const std::string& device_id,
                             const std::string& device_name,
                             const std::string& ip,
                             uint16_t port,
                             uint64_t timestamp) {
    json msg;
    msg["type"]        = MsgType::DEVICE_BROADCAST;
    msg["device_id"]   = device_id;
    msg["device_name"] = device_name;
    msg["ip"]          = ip;
    msg["port"]        = port;
    msg["timestamp"]   = timestamp;
    return msg;
}

json build_device_offline(const std::string& device_id) {
    json msg;
    msg["type"]      = MsgType::DEVICE_OFFLINE;
    msg["device_id"] = device_id;
    return msg;
}

json build_device_hello(const std::string& device_id,
                         const std::string& device_name,
                         const std::string& ip,
                         uint16_t port,
                         const std::string& public_key) {
    json msg;
    msg["type"]        = MsgType::DEVICE_HELLO;
    msg["device_id"]   = device_id;
    msg["device_name"] = device_name;
    msg["ip"]          = ip;
    msg["port"]        = port;
    if (!public_key.empty()) {
        msg["public_key"] = public_key;
    }
    return msg;
}

json build_text_message(const std::string& text) {
    json msg;
    msg["type"] = MsgType::TEXT_MESSAGE;
    msg["text"] = text;
    return msg;
}

json build_file_request(const std::string& file_id,
                         const std::string& filename,
                         uint64_t file_size,
                         const std::string& checksum,
                         uint32_t chunk_size,
                         uint32_t total_chunks) {
    json msg;
    msg["type"]         = MsgType::FILE_REQUEST;
    msg["file_id"]      = file_id;
    msg["filename"]     = filename;
    msg["file_size"]    = file_size;
    msg["checksum"]     = checksum;
    msg["chunk_size"]   = chunk_size;
    msg["total_chunks"] = total_chunks;
    msg["compression"]  = "zlib";
    return msg;
}

json build_file_response(const std::string& file_id,
                          const std::string& status,
                          uint32_t resume_from_chunk,
                          const std::string& reason) {
    json msg;
    msg["type"]              = MsgType::FILE_RESPONSE;
    msg["file_id"]           = file_id;
    msg["status"]            = status;
    msg["resume_from_chunk"] = resume_from_chunk;
    msg["reason"]            = reason;
    return msg;
}

json build_chunk_ack(const std::string& file_id,
                      uint32_t max_contiguous_chunk) {
    json msg;
    msg["type"]                = MsgType::CHUNK_ACK;
    msg["file_id"]             = file_id;
    msg["max_contiguous_chunk"] = max_contiguous_chunk;
    return msg;
}

json build_control_message(const std::string& type,
                            const std::string& file_id,
                            const std::string& extra) {
    json msg;
    msg["type"]    = type;
    msg["file_id"] = file_id;
    if (!extra.empty()) {
        msg["message"] = extra;
    }
    return msg;
}

// ============================================================
// MAC 报文鉴别码 (MD5 + 共享密钥 + 对端 ECDH 子密钥)
// ============================================================

void sign_message(json& msg, const std::string& peer_ip) {
    json canonical = msg;
    canonical.erase("mac");

    std::string secret = mac_secret_for(peer_ip);
    std::string payload = canonical.dump();
    std::string mac = Utils::md5_data(
        reinterpret_cast<const uint8_t*>((payload + secret).data()),
        payload.size() + secret.size());

    msg["mac"] = mac;
}

bool verify_message(const json& msg, const std::string& peer_ip) {
    if (!msg.contains("mac")) {
        return true;
    }

    json canonical = msg;
    std::string received_mac = canonical["mac"];
    canonical.erase("mac");

    std::string payload = canonical.dump();
    std::string secret = mac_secret_for(peer_ip);
    std::string expected_mac = Utils::md5_data(
        reinterpret_cast<const uint8_t*>((payload + secret).data()),
        payload.size() + secret.size());

    return received_mac == expected_mac;
}

// ============================================================
// 带长度前缀的JSON消息收发 (内部实现)
// ============================================================

static bool send_json_message_raw(SOCKET_FD sock, const json& msg) {
    std::string json_str = msg.dump();

    uint32_t data_len = static_cast<uint32_t>(json_str.size());
    uint32_t net_len = htonl(data_len);

    if (SOCK_SEND(sock, reinterpret_cast<const char*>(&net_len),
                  4, 0) != 4) {
        return false;
    }

    size_t total_sent = 0;
    while (total_sent < json_str.size()) {
        auto sent = SOCK_SEND(sock, json_str.data() + total_sent,
                              static_cast<int>(json_str.size() - total_sent), 0);
        if (sent <= 0) {
            return false;
        }
        total_sent += sent;
    }

    return true;
}

static bool recv_json_message_raw(SOCKET_FD sock, json& msg) {
    uint32_t net_len;
    size_t total_read = 0;
    while (total_read < 4) {
        auto n = SOCK_RECV(sock, reinterpret_cast<char*>(&net_len) + total_read,
                           static_cast<int>(4 - total_read), 0);
        if (n <= 0) {
            return false;
        }
        total_read += n;
    }
    uint32_t data_len = ntohl(net_len);

    constexpr uint32_t MAX_MSG_SIZE = 10 * 1024 * 1024;
    if (data_len > MAX_MSG_SIZE) {
        return false;
    }

    std::vector<char> buffer(data_len + 1);
    total_read = 0;
    while (total_read < data_len) {
        auto n = SOCK_RECV(sock, buffer.data() + total_read,
                           static_cast<int>(data_len - total_read), 0);
        if (n <= 0) {
            return false;
        }
        total_read += n;
    }
    buffer[data_len] = '\0';

    try {
        msg = json::parse(buffer.data());
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

// ============================================================
// 公开接口
// ============================================================

bool send_json_message(SOCKET_FD sock, const json& msg) {
    json signed_msg = msg;
    sign_message(signed_msg, "");
    return send_json_message_raw(sock, signed_msg);
}

bool send_json_message(SOCKET_FD sock, const json& msg, const std::string& peer_ip) {
    json signed_msg = msg;
    sign_message(signed_msg, peer_ip);
    return send_json_message_raw(sock, signed_msg);
}

bool recv_json_message(SOCKET_FD sock, json& msg) {
    if (!recv_json_message_raw(sock, msg)) return false;
    if (!verify_message(msg, "")) return false;
    return true;
}

bool recv_json_message(SOCKET_FD sock, json& msg, const std::string& peer_ip) {
    if (!recv_json_message_raw(sock, msg)) return false;
    if (!verify_message(msg, peer_ip)) return false;
    return true;
}

} // namespace Protocol
