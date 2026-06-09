#include "transfer/transfer_sender.h"
#include "common/protocol.h"
#include "common/utils.h"
#include "common/peer_key.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <netinet/tcp.h>
#include <unistd.h>
#include <arpa/inet.h>

TransferSender::TransferSender()
    : m_transferring(false)
{
}

TransferSender::~TransferSender() {
}

bool TransferSender::send_file(const std::string& target_ip,
                                uint16_t target_port,
                                const std::string& file_path,
                                const std::string& file_id,
                                uint32_t num_threads,
                                ProgressCallback callback) {
    m_transferring = true;
    m_target_ip = target_ip;
    m_metrics = TransferMetrics{};

    FileChunkIO io(file_path, Defaults::CHUNK_SIZE, true);
    if (io.get_file_size() == 0) {
        std::cerr << "[发送] 文件为空或无法打开: " << file_path << std::endl;
        m_transferring = false;
        return false;
    }

    FileMeta meta;
    meta.file_id      = file_id;
    meta.filename     = Utils::get_filename(file_path);
    meta.chunk_size   = Defaults::CHUNK_SIZE;

    std::string temp_path;
    bool compressed = false;
    {
        std::ifstream src(file_path, std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(src)),
                        std::istreambuf_iterator<char>());
        src.close();

        std::string compressed_data = Utils::compress_data(raw);
        if (!compressed_data.empty() && compressed_data.size() < raw.size()) {
            temp_path = "/tmp/p2p_send_compressed_" + file_id;
            std::ofstream tmp(temp_path, std::ios::binary);
            tmp.write(compressed_data.data(), compressed_data.size());
            tmp.close();
            meta.file_size    = compressed_data.size();
            meta.total_chunks = (static_cast<uint32_t>(compressed_data.size()) + Defaults::CHUNK_SIZE - 1)
                                / Defaults::CHUNK_SIZE;
            compressed = true;
            std::cout << "[发送] zlib 压缩: " << Utils::format_file_size(raw.size())
                      << " → " << Utils::format_file_size(meta.file_size)
                      << " (" << (100 - meta.file_size * 100 / raw.size()) << "% 节省)" << std::endl;
        } else {
            meta.file_size    = io.get_file_size();
            meta.total_chunks = io.get_total_chunks();
        }
    }

    std::string send_path = compressed ? temp_path : file_path;

    if (meta.total_chunks < num_threads) {
        num_threads = meta.total_chunks > 0 ? meta.total_chunks : 1;
    }

    std::cout << "[发送] 准备发送: " << meta.filename
              << " (" << Utils::format_file_size(meta.file_size) << ")"
              << ", 分片: " << meta.total_chunks
              << ", " << num_threads << " 线程" << std::endl;

    auto start_time = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    std::atomic<uint64_t> global_bytes_sent(0);
    std::atomic<uint32_t> global_chunks_sent(0);
    std::vector<ThreadMetrics> thread_metrics(num_threads);

    for (uint32_t t = 0; t < num_threads; ++t) {
        uint32_t chunks_per = meta.total_chunks / num_threads;
        uint32_t rem = meta.total_chunks % num_threads;
        uint32_t start = t * chunks_per + std::min(t, rem);
        uint32_t end = start + chunks_per + (t < rem ? 1 : 0);

        workers.emplace_back([this, target_ip, target_port, send_path, meta,
                              start, end, t, num_threads,
                              &global_bytes_sent, &global_chunks_sent, callback,
                              &thread_metrics, start_time]() {
            send_chunk_range(target_ip, target_port, send_path, meta,
                             start, end, t, num_threads,
                             global_bytes_sent, global_chunks_sent,
                             thread_metrics[t]);
        });
    }

    if (callback) {
        while (global_chunks_sent.load() < meta.total_chunks) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
            uint64_t bs = global_bytes_sent.load();
            double speed = elapsed > 0 ? (bs * 1000.0 / elapsed) : 0;
            TransferProgress p;
            p.total_chunks = meta.total_chunks;
            p.sent_chunks  = global_chunks_sent.load();
            p.acked_chunks = global_chunks_sent.load();
            p.bytes_sent   = bs;
            p.speed        = speed;
            p.elapsed_ms   = elapsed;
            callback(p);
        }
    }

    for (auto& w : workers) w.join();

    auto end_time = std::chrono::steady_clock::now();
    auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

    if (compressed && !temp_path.empty()) {
        std::remove(temp_path.c_str());
    }

    m_metrics.file_size      = meta.file_size;
    m_metrics.num_threads    = num_threads;
    m_metrics.total_time_us  = static_cast<uint64_t>(total_us);
    m_metrics.throughput_mbps = total_us > 0 ? (meta.file_size * 8.0 / total_us) : 0;
    m_metrics.threads         = std::move(thread_metrics);

    m_transferring = false;

    std::cout << "[发送] 传输完成, 耗时 " << (total_us / 1000) << " ms, "
              << m_metrics.throughput_mbps << " Mbps, "
              << num_threads << " 线程" << std::endl;

    {
        json j;
        j["file_size"] = m_metrics.file_size;
        j["num_threads"] = m_metrics.num_threads;
        j["total_time_us"] = m_metrics.total_time_us;
        j["throughput_mbps"] = m_metrics.throughput_mbps;
        json threads_arr = json::array();
        for (auto& tm : m_metrics.threads) {
            json t;
            t["connection_time_us"] = tm.connection_time_us;
            t["first_byte_time_us"] = tm.first_byte_time_us;
            t["total_time_us"] = tm.total_time_us;
            t["bytes_sent"] = tm.bytes_sent;
            t["start_chunk"] = tm.start_chunk;
            t["end_chunk"] = tm.end_chunk;
            json samples = json::array();
            for (auto& s : tm.speed_samples) {
                samples.push_back({s.first, s.second});
            }
            t["speed_samples"] = samples;
            threads_arr.push_back(t);
        }
        j["threads"] = threads_arr;
        std::ofstream metrics_file("/tmp/p2p_metrics.json");
        metrics_file << j.dump(2);
        metrics_file.close();
    }

    return true;
}

bool TransferSender::send_file_header(SOCKET_FD sock, const FileMeta& meta) {
    json header;
    header["type"]         = "FILE_HEADER";
    header["file_id"]      = meta.file_id;
    header["filename"]     = meta.filename;
    header["file_size"]    = meta.file_size;
    header["chunk_size"]   = meta.chunk_size;
    header["total_chunks"] = meta.total_chunks;
    header["compression"]  = "zlib";

    std::string secret = PeerKey::get(m_target_ip);
    if (!secret.empty()) {
        return Protocol::encrypted_send_json(sock, header, m_target_ip);
    }

    char type_marker = 'J';
    if (SOCK_SEND(sock, &type_marker, 1, 0) != 1) return false;
    return Protocol::send_json_message(sock, header);
}

static bool send_range_header(SOCKET_FD sock, const FileMeta& meta,
                               uint32_t start_chunk, uint32_t end_chunk,
                               uint32_t conn_index, uint32_t total_connections,
                               const std::string& target_ip) {
    json hdr;
    hdr["type"]              = "FILE_RANGE";
    hdr["file_id"]           = meta.file_id;
    hdr["filename"]          = meta.filename;
    hdr["file_size"]         = meta.file_size;
    hdr["chunk_size"]        = meta.chunk_size;
    hdr["total_chunks"]      = meta.total_chunks;
    hdr["compression"]       = "zlib";
    hdr["start_chunk"]       = start_chunk;
    hdr["end_chunk"]         = end_chunk;
    hdr["conn_index"]        = conn_index;
    hdr["total_connections"] = total_connections;

    std::string secret = PeerKey::get(target_ip);
    if (!secret.empty()) {
        char type_marker = 'E';
        if (SOCK_SEND(sock, &type_marker, 1, 0) != 1) return false;
        return Protocol::encrypted_send_json(sock, hdr, target_ip);
    }
    char type_marker = 'J';
    if (SOCK_SEND(sock, &type_marker, 1, 0) != 1) return false;
    return Protocol::send_json_message(sock, hdr);
}

bool TransferSender::send_chunk(SOCKET_FD sock, const Chunk& chunk) {
    std::vector<uint8_t> data = chunk.serialize();

    std::string secret = PeerKey::get(m_target_ip);
    if (!secret.empty()) {
        char type_marker = 'D';
        if (SOCK_SEND(sock, &type_marker, 1, 0) != 1) return false;
        return Protocol::encrypted_send_data(sock, data.data(), data.size(), m_target_ip);
    }

    char type_marker = 'C';
    if (SOCK_SEND(sock, &type_marker, 1, 0) != 1) return false;

    size_t total_sent = 0;
    while (total_sent < data.size()) {
        auto n = SOCK_SEND(sock,
            reinterpret_cast<const char*>(data.data()) + total_sent,
            static_cast<int>(data.size() - total_sent), 0);
        if (n <= 0) return false;
        total_sent += n;
    }
    return true;
}

bool TransferSender::send_chunk_range(const std::string& target_ip,
                                       uint16_t target_port,
                                       const std::string& file_path,
                                       const FileMeta& meta,
                                       uint32_t start_chunk,
                                       uint32_t end_chunk,
                                       uint32_t conn_index,
                                       uint32_t total_connections,
                                       std::atomic<uint64_t>& bytes_sent,
                                       std::atomic<uint32_t>& chunks_sent,
                                       ThreadMetrics& metrics) {
    metrics = ThreadMetrics{};
    metrics.start_chunk = start_chunk;
    metrics.end_chunk   = end_chunk;

    auto t0 = std::chrono::steady_clock::now();

    SOCKET_FD sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET_FD) {
        std::cerr << "[发送线程" << conn_index << "] socket() 失败" << std::endl;
        return false;
    }

    int send_buf = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&send_buf, sizeof(send_buf));

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    if (NetworkUtils::enable_bbr(sock)) {
        std::cout << "[发送线程" << conn_index << "] BBR 已启用" << std::endl;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(target_port);
    inet_pton(AF_INET, target_ip.c_str(), &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[发送线程" << conn_index << "] connect() 失败: "
                  << NetworkUtils::get_last_error_string() << std::endl;
        CLOSE_SOCKET(sock);
        return false;
    }

    auto t_connected = std::chrono::steady_clock::now();
    metrics.connection_time_us = std::chrono::duration_cast<std::chrono::microseconds>(t_connected - t0).count();

    if (!send_range_header(sock, meta, start_chunk, end_chunk, conn_index, total_connections, m_target_ip)) {
        std::cerr << "[发送线程" << conn_index << "] 发送范围头失败" << std::endl;
        CLOSE_SOCKET(sock);
        return false;
    }

    FileChunkIO io(file_path, Defaults::CHUNK_SIZE, true);
    uint64_t local_bytes = 0;
    auto first_sent = false;
    auto last_sample = t_connected;

    for (uint32_t i = start_chunk; i < end_chunk; ++i) {
        Chunk chunk;
        if (!io.read_chunk(i, meta.file_id, meta.total_chunks, chunk)) {
            std::cerr << "[发送线程" << conn_index << "] read_chunk 失败: " << i << std::endl;
            CLOSE_SOCKET(sock);
            return false;
        }
        if (!send_chunk(sock, chunk)) {
            std::cerr << "[发送线程" << conn_index << "] send_chunk 失败: " << i << std::endl;
            CLOSE_SOCKET(sock);
            return false;
        }

        local_bytes += chunk.data_size;
        bytes_sent.fetch_add(chunk.data_size, std::memory_order_relaxed);
        chunks_sent.fetch_add(1, std::memory_order_relaxed);

        if (!first_sent) {
            first_sent = true;
            auto t_first = std::chrono::steady_clock::now();
            metrics.first_byte_time_us = std::chrono::duration_cast<std::chrono::microseconds>(t_first - t0).count();
        }

        auto now = std::chrono::steady_clock::now();
        auto since = std::chrono::duration_cast<std::chrono::microseconds>(now - last_sample).count();
        if (since >= 100000) {
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - t_connected).count();
            double speed = elapsed > 0 ? (local_bytes * 1000000.0 / elapsed) : 0;
            metrics.speed_samples.push_back({static_cast<uint64_t>(elapsed), speed});
            last_sample = now;
        }
    }

    json done_msg;
    done_msg["type"]     = "RANGE_DONE";
    done_msg["file_id"]  = meta.file_id;
    done_msg["conn_index"] = conn_index;
    done_msg["start_chunk"] = start_chunk;
    done_msg["end_chunk"]   = end_chunk;

    std::string secret = PeerKey::get(m_target_ip);
    if (!secret.empty()) {
        char type_marker = 'E';
        SOCK_SEND(sock, &type_marker, 1, 0);
        Protocol::encrypted_send_json(sock, done_msg, m_target_ip);
    } else {
        char type_marker = 'J';
        SOCK_SEND(sock, &type_marker, 1, 0);
        Protocol::send_json_message(sock, done_msg);
    }

    {
        json ack;
        if (!secret.empty()) {
            char marker;
            if (SOCK_RECV(sock, &marker, 1, 0) == 1 && (marker == 'E' || marker == 'J')) {
                if (marker == 'E') {
                    Protocol::encrypted_recv_json(sock, ack, m_target_ip);
                } else {
                    Protocol::recv_json_message(sock, ack);
                }
            }
        } else {
            char marker;
            if (SOCK_RECV(sock, &marker, 1, 0) == 1 && marker == 'J') {
                Protocol::recv_json_message(sock, ack);
            }
        }
    }

    auto t_end = std::chrono::steady_clock::now();
    metrics.total_time_us  = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t0).count();
    metrics.bytes_sent     = local_bytes;

    CLOSE_SOCKET(sock);
    return true;
}
