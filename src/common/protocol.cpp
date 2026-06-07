// ============================================================
// 应用层协议实现
// ============================================================

#include "common/protocol.h"
#include "common/platform.h"
#include "common/utils.h"
#include <cstring>
#include <vector>
#include <cstdint>

namespace Protocol {

constexpr const char* MAC_SECRET = "p2p-transfer-secret-2024";

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
                         uint16_t port) {
    json msg;
    msg["type"]        = MsgType::DEVICE_HELLO;
    msg["device_id"]   = device_id;
    msg["device_name"] = device_name;
    msg["ip"]          = ip;
    msg["port"]        = port;
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
// MAC 报文鉴别码 (MD5 + 共享密钥)
// ============================================================

void sign_message(json& msg) {
    json canonical = msg;
    canonical.erase("mac");

    std::string payload = canonical.dump();
    std::string mac = Utils::md5_data(
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    mac = Utils::md5_data(
        reinterpret_cast<const uint8_t*>((payload + MAC_SECRET).data()),
        payload.size() + strlen(MAC_SECRET));

    msg["mac"] = mac;
}

bool verify_message(const json& msg) {
    if (!msg.contains("mac")) {
        // 兼容旧版本: 无 MAC 的消息仍接受
        return true;
    }

    json canonical = msg;
    std::string received_mac = canonical["mac"];
    canonical.erase("mac");

    std::string payload = canonical.dump();
    std::string expected_mac = Utils::md5_data(
        reinterpret_cast<const uint8_t*>((payload + MAC_SECRET).data()),
        payload.size() + strlen(MAC_SECRET));

    return received_mac == expected_mac;
}

// ============================================================
// 带长度前缀的JSON消息收发
// 格式: [4字节消息体长度 (网络字节序, uint32_t)] + [JSON字符串 (UTF-8)]
// ============================================================

/**
 * @brief 发送JSON消息 (带长度前缀, 防止TCP粘包)
 * @param sock socket描述符
 * @param msg  JSON消息对象
 * @return 是否发送成功
 *
 * 发送流程:
 * 1. 将JSON对象序列化为字符串
 * 2. 计算字符串长度, 转换为4字节的网络字节序 (大端)
 * 3. 先发送4字节长度, 再发送JSON字符串
 *
 * 为什么需要长度前缀?
 * TCP是流式协议, 发送方连续发送两条消息时, 接收方可能一次recv收到两条。
 * 有了长度前缀后, 接收方先读4字节获取长度, 再精确读取该长度的数据,
 * 剩余数据保留在缓冲区供下一条消息读取。
 */
bool send_json_message(SOCKET_FD sock, const json& msg) {
    json signed_msg = msg;
    sign_message(signed_msg);

    std::string json_str = signed_msg.dump();

    // 构造长度前缀 + 数据体
    uint32_t data_len = static_cast<uint32_t>(json_str.size());
    uint32_t net_len = htonl(data_len);  // 主机字节序 -> 网络字节序

    // 先发送4字节长度
    if (SOCK_SEND(sock, reinterpret_cast<const char*>(&net_len),
                  4, 0) != 4) {
        return false;
    }

    // 再发送JSON数据
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

/**
 * @brief 接收JSON消息 (带长度前缀解析)
 * @param sock socket描述符
 * @param msg  输出: 解析后的JSON对象
 * @return 是否接收成功
 *
 * 接收流程:
 * 1. 先读取4字节, 解析为网络字节序的长度值
 * 2. 精确读取指定长度的JSON数据
 * 3. 将JSON字符串反序列化为JSON对象
 */
bool recv_json_message(SOCKET_FD sock, json& msg) {
    // 1. 读取4字节长度前缀
    uint32_t net_len;
    size_t total_read = 0;
    while (total_read < 4) {
        auto n = SOCK_RECV(sock, reinterpret_cast<char*>(&net_len) + total_read,
                           static_cast<int>(4 - total_read), 0);
        if (n <= 0) {
            return false;  // 连接关闭或出错
        }
        total_read += n;
    }
    uint32_t data_len = ntohl(net_len);

    // 安全检查: 消息长度不能过大 (防止恶意攻击或协议错误)
    constexpr uint32_t MAX_MSG_SIZE = 10 * 1024 * 1024;  // 10MB
    if (data_len > MAX_MSG_SIZE) {
        return false;
    }

    // 2. 读取JSON数据体
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

    // 3. 解析JSON
    try {
        msg = json::parse(buffer.data());
        // 4. 校验 MAC
        if (!verify_message(msg)) return false;
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

} // namespace Protocol
