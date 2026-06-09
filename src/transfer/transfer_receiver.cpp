#include "transfer/transfer_receiver.h"
#include "common/protocol.h"
#include "common/utils.h"
#include "common/peer_key.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

constexpr int LISTEN_BACKLOG = 64;

std::mutex TransferReceiver::s_inbound_mutex;
std::map<std::string, InboundTransfer> TransferReceiver::s_inbound;

TransferReceiver::TransferReceiver(uint16_t port)
    : m_port(port)
    , m_save_dir("./received_files")
    , m_listen_socket(INVALID_SOCKET_FD)
    , m_running(false)
{
}

TransferReceiver::~TransferReceiver() {
    stop();
}

bool TransferReceiver::start() {
    if (m_running.load()) return true;
    if (!create_listen_socket()) return false;
    m_running = true;
    m_accept_thread = std::thread(&TransferReceiver::accept_loop, this);
    std::cout << "[接收] 传输接收服务启动 (端口 " << m_port << ")" << std::endl;
    std::cout << "[接收] 保存目录: " << m_save_dir << std::endl;
    return true;
}

void TransferReceiver::stop() {
    if (!m_running.load()) return;
    std::cout << "[接收] 正在停止传输接收服务..." << std::endl;
    m_running = false;
    if (m_listen_socket != INVALID_SOCKET_FD) {
        CLOSE_SOCKET(m_listen_socket);
        m_listen_socket = INVALID_SOCKET_FD;
    }
    if (m_accept_thread.joinable()) m_accept_thread.join();
    {
        std::lock_guard<std::mutex> lock(m_handler_mutex);
        for (auto& t : m_handler_threads) {
            if (t.joinable()) t.join();
        }
        m_handler_threads.clear();
    }
    std::cout << "[接收] 传输接收服务已停止" << std::endl;
}

void TransferReceiver::set_on_receive_complete(ReceiveCompleteCallback callback) {
    m_complete_cb = std::move(callback);
}

void TransferReceiver::set_on_receive_start(ReceiveStartCallback callback) {
    m_start_cb = std::move(callback);
}

bool TransferReceiver::create_listen_socket() {
    m_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listen_socket == INVALID_SOCKET_FD) {
        std::cerr << "[接收] socket() 失败: " << NetworkUtils::get_last_error_string() << std::endl;
        return false;
    }
    NetworkUtils::set_reuse_addr(m_listen_socket);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(m_listen_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[接收] bind() 失败: " << NetworkUtils::get_last_error_string() << std::endl;
        CLOSE_SOCKET(m_listen_socket);
        m_listen_socket = INVALID_SOCKET_FD;
        return false;
    }
    if (listen(m_listen_socket, LISTEN_BACKLOG) < 0) {
        std::cerr << "[接收] listen() 失败" << std::endl;
        CLOSE_SOCKET(m_listen_socket);
        m_listen_socket = INVALID_SOCKET_FD;
        return false;
    }
    return true;
}

void TransferReceiver::accept_loop() {
    std::cout << "[接收] 接受连接线程启动" << std::endl;
    while (m_running.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(m_listen_socket, &read_fds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int sel = select((int)(m_listen_socket + 1), &read_fds, nullptr, nullptr, &tv);
        if (sel < 0) break;
        if (sel == 0) continue;
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        SOCKET_FD client_sock = accept(m_listen_socket, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock == INVALID_SOCKET_FD) break;
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        std::string sender_ip(ip_str);
        std::cout << "[接收] 发送方已连接: " << sender_ip << std::endl;
        std::thread handler([this, client_sock, sender_ip]() {
            handle_receive(client_sock, sender_ip);
        });
        {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            m_handler_threads.erase(
                std::remove_if(m_handler_threads.begin(), m_handler_threads.end(),
                    [](std::thread& t) { return !t.joinable(); }),
                m_handler_threads.end());
            m_handler_threads.push_back(std::move(handler));
        }
    }
    std::cout << "[接收] 接受连接线程退出" << std::endl;
}

static json recv_json_any(SOCKET_FD sock, const std::string& sender_ip) {
    char marker;
    if (SOCK_RECV(sock, &marker, 1, 0) != 1) return {};
    json result;
    if (marker == 'E') {
        if (!Protocol::encrypted_recv_json(sock, result, sender_ip)) return {};
    } else if (marker == 'J') {
        if (!Protocol::recv_json_message(sock, result)) return {};
    } else {
        return {};
    }
    return result;
}

void TransferReceiver::handle_receive(SOCKET_FD client_sock, const std::string& sender_ip) {
    json hdr = recv_json_any(client_sock, sender_ip);
    if (hdr.is_null()) {
        std::cerr << "[接收] 接收消息头失败" << std::endl;
        CLOSE_SOCKET(client_sock);
        return;
    }

    std::string type = hdr.value("type", "");
    if (type == "FILE_RANGE") {
        handle_single_range(client_sock, sender_ip, hdr);
        return;
    }

    if (type != "FILE_HEADER") {
        std::cerr << "[接收] 未知消息类型: " << type << std::endl;
        CLOSE_SOCKET(client_sock);
        return;
    }

    FileMeta meta;
    meta.file_id      = hdr.value("file_id", "");
    meta.filename     = hdr.value("filename", "");
    meta.file_size    = hdr.value("file_size", uint64_t(0));
    meta.chunk_size   = hdr.value("chunk_size", uint32_t(0));
    meta.total_chunks = hdr.value("total_chunks", uint32_t(0));
    meta.compression  = hdr.value("compression", "");

    std::cout << "[接收] 接收文件: " << meta.filename
              << " (" << Utils::format_file_size(meta.file_size) << ")"
              << ", 分片: " << meta.total_chunks << " (单连接)" << std::endl;

    if (m_start_cb) {
        m_start_cb(meta.file_id, meta.filename, meta.file_size,
                   meta.total_chunks, sender_ip);
    }

    std::string save_path = m_save_dir + "/" + meta.filename;
    std::string temp_path = save_path + ".tmp";

    int fd = open(temp_path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        std::cerr << "[接收] 无法创建临时文件: " << temp_path << std::endl;
        CLOSE_SOCKET(client_sock);
        return;
    }
    struct stat st;
    if (fstat(fd, &st) == 0) {
        if (static_cast<uint64_t>(st.st_size) < meta.file_size) {
            (void) ftruncate(fd, static_cast<off_t>(meta.file_size));
        }
    }
    close(fd);

    FileChunkIO io(save_path, meta.chunk_size, false);
    if (io.get_total_chunks() == 0) {
        std::cerr << "[接收] 文件初始化失败: total_chunks=0" << std::endl;
        CLOSE_SOCKET(client_sock);
        return;
    }

    uint32_t chunk_count = 0;
    while (chunk_count < meta.total_chunks) {
        Chunk chunk;
        if (!recv_chunk(client_sock, chunk, sender_ip)) {
            std::cerr << "[接收] 接收分片失败, 已接收: " << chunk_count << std::endl;
            CLOSE_SOCKET(client_sock);
            return;
        }
        if (chunk.chunk_index == 0xFFFFFFFF) break;
        if (!io.write_chunk(chunk)) {
            std::cerr << "[接收] 写入分片失败: chunk=" << chunk.chunk_index << std::endl;
            CLOSE_SOCKET(client_sock);
            return;
        }
        ++chunk_count;
    }

    uint32_t final_contiguous = io.get_max_contiguous_chunk();
    send_ack(client_sock, meta.file_id, final_contiguous, sender_ip);

    if (final_contiguous == meta.total_chunks - 1) {
        if (io.commit_received_file()) {
            if (meta.compression == "zlib") {
                std::ifstream comp_file(save_path, std::ios::binary);
                std::string comp_data((std::istreambuf_iterator<char>(comp_file)),
                                      std::istreambuf_iterator<char>());
                comp_file.close();
                std::string raw_data = Utils::decompress_data(comp_data);
                if (!raw_data.empty()) {
                    std::ofstream out(save_path, std::ios::binary);
                    out.write(raw_data.data(), raw_data.size());
                    out.close();
                    std::cout << "[接收] zlib 解压: " << Utils::format_file_size(comp_data.size())
                              << " -> " << Utils::format_file_size(raw_data.size()) << std::endl;
                }
            }
            std::cout << "[接收] 文件接收完成: " << meta.filename << std::endl;
            if (m_complete_cb) m_complete_cb(meta.file_id, save_path, true);
        }
    } else {
        std::cerr << "[接收] 文件不完整: " << final_contiguous + 1
                  << "/" << meta.total_chunks << std::endl;
        if (m_complete_cb) m_complete_cb(meta.file_id, save_path, false);
    }

    CLOSE_SOCKET(client_sock);
}

bool TransferReceiver::handle_single_range(SOCKET_FD client_sock, const std::string& sender_ip, const json& hdr) {
    std::string file_id        = hdr.value("file_id", "");
    std::string filename       = hdr.value("filename", "");
    uint64_t    file_size      = hdr.value("file_size", uint64_t(0));
    uint32_t    chunk_size     = hdr.value("chunk_size", Defaults::CHUNK_SIZE);
    uint32_t    total_chunks   = hdr.value("total_chunks", uint32_t(0));
    std::string compression    = hdr.value("compression", "");
    uint32_t    start_chunk    = hdr.value("start_chunk", uint32_t(0));
    uint32_t    end_chunk      = hdr.value("end_chunk", uint32_t(0));
    uint32_t    conn_index     = hdr.value("conn_index", uint32_t(0));
    uint32_t    total_conns    = hdr.value("total_connections", uint32_t(1));

    std::cout << "[接收] " << filename << " 范围 #" << conn_index
              << " [" << start_chunk << "-" << end_chunk << ") "
              << Utils::format_file_size((end_chunk - start_chunk) * (uint64_t)chunk_size)
              << " (" << conn_index + 1 << "/" << total_conns << ")" << std::endl;

    std::string save_path = m_save_dir + "/" + filename;
    std::string temp_path = save_path + ".tmp";

    {
        std::lock_guard<std::mutex> lock(s_inbound_mutex);
        auto it = s_inbound.find(file_id);
        if (it == s_inbound.end()) {
            InboundTransfer ib;
            ib.meta.file_id        = file_id;
            ib.meta.filename       = filename;
            ib.meta.file_size      = file_size;
            ib.meta.chunk_size     = chunk_size;
            ib.meta.total_chunks   = total_chunks;
            ib.meta.compression    = compression;
            ib.total_connections   = total_conns;
            ib.finished_connections = 0;
            ib.received_bitmap.resize(total_chunks, false);
            ib.received_count      = 0;
            ib.save_path           = save_path;
            ib.temp_path           = temp_path;
            ib.committed           = false;
            ib.start_fired         = false;
            s_inbound[file_id] = ib;
        }
    }

    {
        std::lock_guard<std::mutex> lock(s_inbound_mutex);
        auto& ib = s_inbound[file_id];
        if (!ib.start_fired) {
            ib.start_fired = true;
            if (m_start_cb) {
                m_start_cb(file_id, filename, file_size, total_chunks, sender_ip);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(s_inbound_mutex);
        auto& ib = s_inbound[file_id];
        if (!ib.committed) {
            int fd = open(temp_path.c_str(), O_WRONLY | O_CREAT, 0644);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0) {
                    if (static_cast<uint64_t>(st.st_size) < file_size) {
                        (void) ftruncate(fd, static_cast<off_t>(file_size));
                    }
                }
                close(fd);
            }
        }
    }

    FileChunkIO io(save_path, chunk_size, false);

    for (uint32_t i = start_chunk; i < end_chunk; ++i) {
        Chunk chunk;
        if (!recv_chunk(client_sock, chunk, sender_ip)) {
            std::cerr << "[接收] 接收分片失败: chunk=" << i << std::endl;
            CLOSE_SOCKET(client_sock);
            return false;
        }
        if (chunk.chunk_index == 0xFFFFFFFF) break;
        if (!io.write_chunk(chunk)) {
            std::cerr << "[接收] 写入分片失败: chunk=" << chunk.chunk_index << std::endl;
            CLOSE_SOCKET(client_sock);
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(s_inbound_mutex);
            auto& ib = s_inbound[file_id];
            if (chunk.chunk_index < ib.received_bitmap.size() && !ib.received_bitmap[chunk.chunk_index]) {
                ib.received_bitmap[chunk.chunk_index] = true;
                ib.received_count++;
            }
        }
    }

    {
        json done_msg;
        done_msg["type"]     = "RANGE_ACK";
        done_msg["file_id"]  = file_id;
        done_msg["status"]   = "ok";
        done_msg["conn_index"] = conn_index;
        std::string secret = PeerKey::get(sender_ip);
        if (!secret.empty()) {
            char marker = 'E';
            SOCK_SEND(client_sock, &marker, 1, 0);
            Protocol::encrypted_send_json(client_sock, done_msg, sender_ip);
        } else {
            char marker = 'J';
            SOCK_SEND(client_sock, &marker, 1, 0);
            Protocol::send_json_message(client_sock, done_msg);
        }
    }

    CLOSE_SOCKET(client_sock);

    {
        std::lock_guard<std::mutex> lock(s_inbound_mutex);
        auto& ib = s_inbound[file_id];
        ib.finished_connections++;

        if (ib.finished_connections >= ib.total_connections && !ib.committed) {
            uint32_t received = ib.received_count;
            if (received >= ib.meta.total_chunks) {
                FileChunkIO commit_io(ib.save_path, ib.meta.chunk_size, false);
                if (commit_io.commit_received_file()) {
                    if (ib.meta.compression == "zlib") {
                        std::ifstream comp_file(ib.save_path, std::ios::binary);
                        std::string comp_data((std::istreambuf_iterator<char>(comp_file)),
                                              std::istreambuf_iterator<char>());
                        comp_file.close();
                        std::string raw_data = Utils::decompress_data(comp_data);
                        if (!raw_data.empty()) {
                            std::ofstream out(ib.save_path, std::ios::binary);
                            out.write(raw_data.data(), raw_data.size());
                            out.close();
                        }
                    }
                    std::cout << "[接收] 文件接收完成: " << ib.meta.filename << std::endl;
                    if (m_complete_cb) m_complete_cb(file_id, ib.save_path, true);
                }
            } else {
                std::cerr << "[接收] 文件不完整: " << received
                          << "/" << ib.meta.total_chunks << std::endl;
                if (m_complete_cb) m_complete_cb(file_id, ib.save_path, false);
            }
            ib.committed = true;
        }
    }

    return true;
}

bool TransferReceiver::recv_file_header(SOCKET_FD sock, FileMeta& meta, const std::string& sender_ip) {
    json hdr = recv_json_any(sock, sender_ip);
    if (hdr.is_null()) return false;
    std::string type = hdr.value("type", "");
    if (type != "FILE_HEADER") return false;
    meta.file_id      = hdr.value("file_id", "");
    meta.filename     = hdr.value("filename", "");
    meta.file_size    = hdr.value("file_size", uint64_t(0));
    meta.chunk_size   = hdr.value("chunk_size", uint32_t(0));
    meta.total_chunks = hdr.value("total_chunks", uint32_t(0));
    meta.compression  = hdr.value("compression", "");
    std::cout << "[接收] 文件头: " << meta.filename
              << " size=" << meta.file_size
              << " chunks=" << meta.total_chunks << std::endl;
    return true;
}

bool TransferReceiver::recv_chunk(SOCKET_FD sock, Chunk& chunk, const std::string& sender_ip) {
    char marker;
    if (SOCK_RECV(sock, &marker, 1, 0) != 1) return false;

    if (marker == 'D') {
        chunk.chunk_index = 0;
        std::vector<uint8_t> data;
        if (!Protocol::encrypted_recv_data(sock, data, 0, sender_ip)) return false;
        if (!chunk.deserialize(data.data(), data.size())) return false;
        return true;
    }

    if (marker == 'J' || marker == 'E') {
        chunk.chunk_index = 0xFFFFFFFF;
        chunk.total_chunks = 0xFFFFFFFF;
        return true;
    }

    if (marker != 'C') {
        std::cerr << "[接收] 分片类型标记错误: " << (int)marker << std::endl;
        return false;
    }

    uint8_t header[CHUNK_HEADER_SIZE];
    size_t total_read = 0;
    while (total_read < CHUNK_HEADER_SIZE) {
        auto n = SOCK_RECV(sock, reinterpret_cast<char*>(header) + total_read,
                           static_cast<int>(CHUNK_HEADER_SIZE - total_read), 0);
        if (n <= 0) return false;
        total_read += n;
    }

    uint32_t data_size    = ((uint32_t)header[8] << 24)  |
                            ((uint32_t)header[9] << 16)  |
                            ((uint32_t)header[10] << 8)  |
                            (uint32_t)header[11];
    uint32_t file_id_len  = ((uint32_t)header[12] << 24) |
                            ((uint32_t)header[13] << 16) |
                            ((uint32_t)header[14] << 8)  |
                            (uint32_t)header[15];

    if (data_size > MAX_CHUNK_DATA_SIZE || file_id_len > 256) {
        std::cerr << "[接收] 分片头部数据异常: data_size=" << data_size
                  << " file_id_len=" << file_id_len << std::endl;
        return false;
    }

    size_t remaining = file_id_len + data_size;
    std::vector<uint8_t> body(remaining);
    total_read = 0;
    while (total_read < remaining) {
        auto n = SOCK_RECV(sock, reinterpret_cast<char*>(body.data()) + total_read,
                           static_cast<int>(remaining - total_read), 0);
        if (n <= 0) return false;
        total_read += n;
    }

    std::vector<uint8_t> full_buffer(CHUNK_HEADER_SIZE + remaining);
    memcpy(full_buffer.data(), header, CHUNK_HEADER_SIZE);
    memcpy(full_buffer.data() + CHUNK_HEADER_SIZE, body.data(), remaining);

    return chunk.deserialize(full_buffer.data(), full_buffer.size());
}

bool TransferReceiver::send_ack(SOCKET_FD sock, const std::string& file_id,
                                 uint32_t max_contiguous_chunk, const std::string& sender_ip) {
    std::string secret = PeerKey::get(sender_ip);
    if (!secret.empty()) {
        char marker = 'E';
        if (SOCK_SEND(sock, &marker, 1, 0) != 1) return false;
        json ack = Protocol::build_chunk_ack(file_id, max_contiguous_chunk);
        return Protocol::encrypted_send_json(sock, ack, sender_ip);
    }
    char marker = 'J';
    if (SOCK_SEND(sock, &marker, 1, 0) != 1) return false;
    json ack = Protocol::build_chunk_ack(file_id, max_contiguous_chunk);
    return Protocol::send_json_message(sock, ack);
}
