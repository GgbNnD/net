#pragma once

// ============================================================
// 文件传输接收方 (TCP)
// 功能: 监听传输端口, 接收文件分片, 写入磁盘
// ============================================================

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

/**
 * @brief 接收完成回调
 * @param file_id   传输任务ID
 * @param file_path 保存的文件路径
 * @param success   是否成功
 */
using ReceiveCompleteCallback = std::function<void(const std::string& file_id,
                                                    const std::string& file_path,
                                                    bool success)>;

/**
 * @brief 接收开始回调 (收到文件头时触发, 用于创建传输任务)
 */
using ReceiveStartCallback = std::function<void(const std::string& file_id,
                                                  const std::string& filename,
                                                  uint64_t file_size,
                                                  uint32_t total_chunks,
                                                  const std::string& sender_ip)>;

/**
 * @class TransferReceiver
 * @brief 文件传输接收方
 *
 * 工作流程:
 * 1. 监听传输端口 (8890), 等待发送方连接
 * 2. 接收文件头信息 (JSON), 初始化 FileChunkIO
 * 3. 循环接收分片 (二进制格式), 写入临时文件
 * 4. 定期发送 ACK 确认
 * 5. 接收完成后验证文件完整性, 提交文件
 *
 * 支持并发接收: 每个接收任务在独立线程中处理
 */
class TransferReceiver {
public:
    TransferReceiver(uint16_t port = Defaults::TRANSFER_PORT);
    ~TransferReceiver();

    TransferReceiver(const TransferReceiver&) = delete;
    TransferReceiver& operator=(const TransferReceiver&) = delete;

    /**
     * @brief 启动接收服务 (监听传输端口)
     * @return 是否成功
     */
    bool start();

    /**
     * @brief 停止接收服务
     */
    void stop();

    /**
     * @brief 是否正在运行
     */
    bool is_running() const { return m_running.load(); }

    /**
     * @brief 设置接收完成回调
     * @param callback 回调函数
     */
    void set_on_receive_complete(ReceiveCompleteCallback callback);

    /**
     * @brief 设置接收开始回调 (收到文件头时触发)
     */
    void set_on_receive_start(ReceiveStartCallback callback);

    /**
     * @brief 获取保存接收文件的目录
     */
    void set_save_directory(const std::string& dir) { m_save_dir = dir; }

private:
    uint16_t m_port;                    // 监听端口
    std::string m_save_dir;             // 文件保存目录
    SOCKET_FD m_listen_socket;          // 监听Socket
    std::atomic<bool> m_running;        // 运行状态
    std::thread m_accept_thread;        // 接受连接线程
    std::vector<std::thread> m_handler_threads;  // 处理线程列表
    std::mutex m_handler_mutex;

    ReceiveCompleteCallback m_complete_cb;  // 完成回调
    ReceiveStartCallback    m_start_cb;     // 开始回调

    /**
     * @brief 创建TCP监听Socket
     */
    bool create_listen_socket();

    /**
     * @brief 接受连接线程
     */
    void accept_loop();

    /**
     * @brief 处理单个接收任务
     * @param client_sock 已建立的连接
     *
     * 处理流程:
     * 1. 接收文件头
     * 2. 初始化 FileChunkIO
     * 3. 循环接收分片, 发送ACK
     * 4. 完成后验证+
     */
    void handle_receive(SOCKET_FD client_sock, const std::string& sender_ip);

    /**
     * @brief 接收文件头信息
     */
    bool recv_file_header(SOCKET_FD sock, FileMeta& meta, const std::string& sender_ip);

    bool recv_chunk(SOCKET_FD sock, Chunk& chunk, const std::string& sender_ip);

    bool send_ack(SOCKET_FD sock, const std::string& file_id,
                  uint32_t max_contiguous_chunk, const std::string& sender_ip);
};
