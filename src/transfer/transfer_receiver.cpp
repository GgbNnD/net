// ============================================================
// 文件传输接收方 - 实现
// ============================================================

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

constexpr int LISTEN_BACKLOG = 5;

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

    if (!create_listen_socket()) {
        return false;
    }

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

    if (m_accept_thread.joinable()) {
        m_accept_thread.join();
    }

    // 等待所有处理线程完成
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

// ----------------------------------------------------------
// 创建监听Socket
// ----------------------------------------------------------
bool TransferReceiver::create_listen_socket() {
    m_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listen_socket == INVALID_SOCKET_FD) {
        std::cerr << "[接收] socket() 失败: "
                  << NetworkUtils::get_last_error_string() << std::endl;
        return false;
    }

    NetworkUtils::set_reuse_addr(m_listen_socket);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(m_listen_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[接收] bind() 失败: "
                  << NetworkUtils::get_last_error_string() << std::endl;
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

// ----------------------------------------------------------
// 接受连接循环
// ----------------------------------------------------------
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
        SOCKET_FD client_sock = accept(m_listen_socket,
                                       (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock == INVALID_SOCKET_FD) break;

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));

        std::string sender_ip(ip_str);

        std::cout << "[接收] 发送方已连接: " << sender_ip << std::endl;

        // 创建处理线程
        std::thread handler([this, client_sock, sender_ip]() {
            handle_receive(client_sock, sender_ip);
        });

        // 清理已完成线程并添加新线程
        {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            m_handler_threads.erase(
                std::remove_if(m_handler_threads.begin(), m_handler_threads.end(),
                    [](std::thread& t) { return !t.joinable(); }),
                m_handler_threads.end()
            );
            m_handler_threads.push_back(std::move(handler));
        }
    }

    std::cout << "[接收] 接受连接线程退出" << std::endl;
}

// ----------------------------------------------------------
// 处理单个接收任务
// ----------------------------------------------------------
void TransferReceiver::handle_receive(SOCKET_FD client_sock, const std::string& sender_ip) {
    // 1. 接收文件头
    FileMeta meta;
    if (!recv_file_header(client_sock, meta, sender_ip)) {
        std::cerr << "[接收] 接收文件头失败" << std::endl;
        CLOSE_SOCKET(client_sock);
        return;
    }

    std::cout << "[接收] 接收文件: " << meta.filename
              << " (" << Utils::format_file_size(meta.file_size) << ")"
              << ", 分片: " << meta.total_chunks << std::endl;

    // 通知上层开始接收 (创建 TransferTask)
    if (m_start_cb) {
        m_start_cb(meta.file_id, meta.filename, meta.file_size,
                   meta.total_chunks, sender_ip);
    }

    // 2. 初始化文件I/O
    std::string save_path = m_save_dir + "/" + meta.filename;
    std::string temp_path = save_path + ".tmp";

    // 确保临时文件存在且大小正确
    int fd = open(temp_path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        std::cerr << "[接收] 无法创建临时文件: " << temp_path << std::endl;
        CLOSE_SOCKET(client_sock);
        return;
    }

    // 获取当前临时文件大小
    struct stat st;
    if (fstat(fd, &st) == 0) {
        if (static_cast<uint64_t>(st.st_size) < meta.file_size) {
            (void) ftruncate(fd, static_cast<off_t>(meta.file_size));
        }
    }
    close(fd);

    // 现在创建 FileChunkIO (临时文件已存在且有正确大小)
    FileChunkIO io(save_path, meta.chunk_size, false);

    if (io.get_total_chunks() == 0) {
        std::cerr << "[接收] 文件初始化失败: total_chunks=0" << std::endl;
        CLOSE_SOCKET(client_sock);
        return;
    }

    // 3. 接收分片循环
    uint32_t last_ack_count = 0;
    uint32_t chunk_count = 0;

    while (chunk_count < meta.total_chunks) {
        Chunk chunk;
        if (!recv_chunk(client_sock, chunk, sender_ip)) {
            std::cerr << "[接收] 接收分片失败, 已接收: " << chunk_count << std::endl;
            CLOSE_SOCKET(client_sock);
            return;
        }

        // 处理控制消息 (JSON, 不是分片)
        if (chunk.chunk_index == 0xFFFFFFFF) {
            // JSON控制消息: TRANSFER_DONE/ERROR 等
            break;
        }

        // 写入分片到文件
        if (!io.write_chunk(chunk)) {
            std::cerr << "[接收] 写入分片失败: chunk=" << chunk.chunk_index << std::endl;
            CLOSE_SOCKET(client_sock);
            return;
        }

        ++chunk_count;

        // 每收到1个分片就发送一次ACK (确保发送方尽快确认)
        if (chunk_count > last_ack_count) {
            uint32_t max_cont = io.get_max_contiguous_chunk();
            send_ack(client_sock, meta.file_id, max_cont, sender_ip);
            last_ack_count = chunk_count;
        }
    }

    // 4. 发送最终ACK
    uint32_t final_contiguous = io.get_max_contiguous_chunk();
    send_ack(client_sock, meta.file_id, final_contiguous, sender_ip);

    // 5. 完成传输, 验证文件
    if (final_contiguous == meta.total_chunks - 1) {
        // 提交文件 (重命名 .tmp)
        if (io.commit_received_file()) {
            // 解压缩
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

            // 验证MD5
            std::string actual_checksum = Utils::md5_file(save_path);
            if (actual_checksum == meta.checksum || meta.checksum.empty()) {
                std::cout << "[接收] 文件接收完成, 校验通过: " << meta.filename << std::endl;
                if (m_complete_cb) {
                    m_complete_cb(meta.file_id, save_path, true);
                }
            } else {
                std::cerr << "[接收] MD5校验失败! 预期: " << meta.checksum
                          << " 实际: " << actual_checksum << std::endl;
                if (m_complete_cb) {
                    m_complete_cb(meta.file_id, save_path, false);
                }
            }
        }
    } else {
        std::cerr << "[接收] 文件不完整: " << final_contiguous + 1
                  << "/" << meta.total_chunks << std::endl;
        if (m_complete_cb) {
            m_complete_cb(meta.file_id, save_path, false);
        }
    }

    CLOSE_SOCKET(client_sock);
}

// ----------------------------------------------------------
// 接收文件头 (JSON消息, 前有类型标记)
// ----------------------------------------------------------
bool TransferReceiver::recv_file_header(SOCKET_FD sock, FileMeta& meta, const std::string& sender_ip) {
    char marker;
    if (SOCK_RECV(sock, &marker, 1, 0) != 1) {
        std::cerr << "[接收] 文件头类型标记读取失败" << std::endl;
        return false;
    }

    if (marker == 'E') {
        json header;
        if (!Protocol::encrypted_recv_json(sock, header, sender_ip))
            return false;
        meta.file_id      = header.value("file_id", "");
        meta.filename     = header.value("filename", "");
        meta.file_size    = header.value("file_size", uint64_t(0));
        meta.checksum     = header.value("checksum", "");
        meta.chunk_size   = header.value("chunk_size", uint32_t(0));
        meta.total_chunks = header.value("total_chunks", uint32_t(0));
        meta.compression  = header.value("compression", "");
        return true;
    }

    if (marker != 'J') {
        std::cerr << "[接收] 文件头类型标记错误: " << (int)marker << std::endl;
        return false;
    }

    json header;
    if (!Protocol::recv_json_message(sock, header)) {
        return false;
    }

    std::string type = header.value("type", "");
    if (type != "FILE_HEADER") {
        std::cerr << "[接收] 预期 FILE_HEADER, 收到: " << type << std::endl;
        return false;
    }

    meta.file_id      = header.value("file_id", "");
    meta.filename     = header.value("filename", "");
    meta.file_size    = header.value("file_size", uint64_t(0));
    meta.checksum     = header.value("checksum", "");
    meta.chunk_size   = header.value("chunk_size", uint32_t(0));
    meta.total_chunks = header.value("total_chunks", uint32_t(0));
    meta.compression  = header.value("compression", "");

    std::cout << "[接收] 文件头: " << meta.filename
              << " size=" << meta.file_size
              << " chunks=" << meta.total_chunks << std::endl;

    return true;
}

// ----------------------------------------------------------
// 接收单个分片 (从二进制流中解析, 前有类型标记)
// ----------------------------------------------------------
bool TransferReceiver::recv_chunk(SOCKET_FD sock, Chunk& chunk, const std::string& sender_ip) {
    char marker;
    if (SOCK_RECV(sock, &marker, 1, 0) != 1) {
        return false;
    }

    if (marker == 'D') {
        chunk.chunk_index = 0;
        std::vector<uint8_t> data;
        if (!Protocol::encrypted_recv_data(sock, data, 0, sender_ip))
            return false;
        if (!chunk.deserialize(data.data(), data.size()))
            return false;
        return true;
    }

    if (marker == 'J') {
        chunk.chunk_index = 0xFFFFFFFF;
        chunk.total_chunks = 0xFFFFFFFF;
        return true;
    }

    if (marker == 'E') {
        chunk.chunk_index = 0xFFFFFFFF;
        chunk.total_chunks = 0xFFFFFFFF;
        return true;
    }

    if (marker != 'C') {
        std::cerr << "[接收] 分片类型标记错误: " << (int)marker << std::endl;
        return false;
    }

    // 先接收分片头部 (16字节)
    uint8_t header[CHUNK_HEADER_SIZE];
    size_t total_read = 0;
    while (total_read < CHUNK_HEADER_SIZE) {
        auto n = SOCK_RECV(sock, reinterpret_cast<char*>(header) + total_read,
                           static_cast<int>(CHUNK_HEADER_SIZE - total_read), 0);
        if (n <= 0) return false;
        total_read += n;
    }

    // 解析头部获取 data_size 和 file_id_len
    uint32_t data_size    = ((uint32_t)header[8] << 24)  |
                            ((uint32_t)header[9] << 16)  |
                            ((uint32_t)header[10] << 8)  |
                            (uint32_t)header[11];
    uint32_t file_id_len  = ((uint32_t)header[12] << 24) |
                            ((uint32_t)header[13] << 16) |
                            ((uint32_t)header[14] << 8)  |
                            (uint32_t)header[15];

    // 安全检查
    if (data_size > MAX_CHUNK_DATA_SIZE || file_id_len > 256) {
        std::cerr << "[接收] 分片头部数据异常: data_size=" << data_size
                  << " file_id_len=" << file_id_len << std::endl;
        return false;
    }

    // 计算剩余需要读取的字节数
    size_t remaining = file_id_len + data_size;
    std::vector<uint8_t> body(remaining);

    total_read = 0;
    while (total_read < remaining) {
        auto n = SOCK_RECV(sock, reinterpret_cast<char*>(body.data()) + total_read,
                           static_cast<int>(remaining - total_read), 0);
        if (n <= 0) return false;
        total_read += n;
    }

    // 构造完整buffer并解析
    std::vector<uint8_t> full_buffer(CHUNK_HEADER_SIZE + remaining);
    memcpy(full_buffer.data(), header, CHUNK_HEADER_SIZE);
    memcpy(full_buffer.data() + CHUNK_HEADER_SIZE, body.data(), remaining);

    return chunk.deserialize(full_buffer.data(), full_buffer.size());
}

// ----------------------------------------------------------
// 发送ACK (JSON消息, 前加类型标记)
// ----------------------------------------------------------
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
