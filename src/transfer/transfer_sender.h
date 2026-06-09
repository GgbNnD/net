#pragma once

#include "common/types.h"
#include "common/platform.h"
#include "transfer/file_io.h"
#include "transfer/chunk.h"
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>

struct TransferProgress {
    uint32_t total_chunks;
    uint32_t sent_chunks;
    uint32_t acked_chunks;
    uint64_t bytes_sent;
    double    speed;
    uint64_t  elapsed_ms;
};

struct ThreadMetrics {
    uint64_t connection_time_us;
    uint64_t first_byte_time_us;
    uint64_t total_time_us;
    uint64_t bytes_sent;
    uint32_t start_chunk;
    uint32_t end_chunk;
    std::vector<std::pair<uint64_t, double>> speed_samples;
};

struct TransferMetrics {
    uint64_t file_size;
    uint32_t num_threads;
    uint64_t total_time_us;
    double   throughput_mbps;
    std::vector<ThreadMetrics> threads;
};

using ProgressCallback = std::function<void(const TransferProgress& progress)>;

class TransferSender {
public:
    TransferSender();
    ~TransferSender();

    TransferSender(const TransferSender&) = delete;
    TransferSender& operator=(const TransferSender&) = delete;

    bool send_file(const std::string& target_ip,
                   uint16_t target_port,
                   const std::string& file_path,
                   const std::string& file_id,
                   uint32_t num_threads = 4,
                   ProgressCallback callback = nullptr);

    bool is_transferring() const { return m_transferring.load(); }

    TransferMetrics get_metrics() const { return m_metrics; }

private:
    std::atomic<bool> m_transferring;
    std::string m_target_ip;
    TransferMetrics m_metrics;
    mutable std::mutex m_metrics_mutex;

    bool send_file_header(SOCKET_FD sock, const FileMeta& meta);
    bool send_chunk(SOCKET_FD sock, const Chunk& chunk);

    bool send_chunk_range(const std::string& target_ip,
                          uint16_t target_port,
                          const std::string& file_path,
                          const FileMeta& meta,
                          uint32_t start_chunk,
                          uint32_t end_chunk,
                          uint32_t conn_index,
                          uint32_t total_connections,
                          std::atomic<uint64_t>& bytes_sent,
                          std::atomic<uint32_t>& chunks_sent,
                          ThreadMetrics& metrics);
};
