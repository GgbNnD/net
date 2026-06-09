#pragma once

#include "common/types.h"
#include "common/platform.h"
#include "transfer/file_io.h"
#include "transfer/chunk.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <mutex>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

using ReceiveCompleteCallback = std::function<void(const std::string& file_id,
                                                    const std::string& file_path,
                                                    bool success)>;

using ReceiveStartCallback = std::function<void(const std::string& file_id,
                                                  const std::string& filename,
                                                  uint64_t file_size,
                                                  uint32_t total_chunks,
                                                  const std::string& sender_ip)>;

struct InboundTransfer {
    FileMeta meta;
    uint32_t total_connections;
    uint32_t finished_connections;
    std::vector<bool> received_bitmap;
    uint32_t received_count;
    std::string save_path;
    std::string temp_path;
    bool committed;
    bool start_fired;
};

class TransferReceiver {
public:
    TransferReceiver(uint16_t port = Defaults::TRANSFER_PORT);
    ~TransferReceiver();

    TransferReceiver(const TransferReceiver&) = delete;
    TransferReceiver& operator=(const TransferReceiver&) = delete;

    bool start();
    void stop();
    bool is_running() const { return m_running.load(); }
    void set_on_receive_complete(ReceiveCompleteCallback callback);
    void set_on_receive_start(ReceiveStartCallback callback);
    void set_save_directory(const std::string& dir) { m_save_dir = dir; }

private:
    uint16_t m_port;
    std::string m_save_dir;
    SOCKET_FD m_listen_socket;
    std::atomic<bool> m_running;
    std::thread m_accept_thread;
    std::vector<std::thread> m_handler_threads;
    std::mutex m_handler_mutex;

    ReceiveCompleteCallback m_complete_cb;
    ReceiveStartCallback    m_start_cb;

    static std::mutex s_inbound_mutex;
    static std::map<std::string, InboundTransfer> s_inbound;

    bool create_listen_socket();
    void accept_loop();
    void handle_receive(SOCKET_FD client_sock, const std::string& sender_ip);

    bool recv_file_header(SOCKET_FD sock, FileMeta& meta, const std::string& sender_ip);
    bool recv_chunk(SOCKET_FD sock, Chunk& chunk, const std::string& sender_ip);
    bool send_ack(SOCKET_FD sock, const std::string& file_id,
                  uint32_t max_contiguous_chunk, const std::string& sender_ip);

    bool handle_single_range(SOCKET_FD client_sock, const std::string& sender_ip, const json& hdr);
};
