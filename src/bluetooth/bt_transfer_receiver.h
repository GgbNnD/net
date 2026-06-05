#pragma once

// ============================================================
// 蓝牙RFCOMM文件传输接收方
// 功能: 替代 TCP TransferReceiver, 在 RFCOMM 通道 2 上监听
//       接收文件分片, 写入磁盘, 发送 ACK
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

// ---- 回调类型 (与原 TransferReceiver 完全兼容) ----
using ReceiveCompleteCallback = std::function<void(const std::string& file_id,
                                                    const std::string& file_path,
                                                    bool success)>;

using ReceiveStartCallback = std::function<void(const std::string& file_id,
                                                 const std::string& filename,
                                                 uint64_t file_size,
                                                 uint32_t total_chunks,
                                                 const std::string& sender_addr)>;

/**
 * @class BtTransferReceiver
 * @brief 蓝牙 RFCOMM 文件传输接收方
 *
 * 工作流程:
 * 1. 监听 RFCOMM 传输通道 (默认 2), 等待发送方连接
 * 2. 接收文件头信息 (JSON, 'J' 标记)
 * 3. 初始化 FileChunkIO (临时文件 + ftruncate)
 * 4. 循环接收分片 (二进制, 'C' 标记), 写入临时文件
 * 5. 定期发送 ACK 确认
 * 6. 接收完成后验证文件完整性, 提交文件
 *
 * 支持并发接收: 每个接收任务在独立线程中处理
 */
class BtTransferReceiver {
public:
    /**
     * @brief 构造函数
     * @param channel RFCOMM 传输通道号 (默认 2)
     */
    explicit BtTransferReceiver(uint8_t channel = 2);

    ~BtTransferReceiver();

    BtTransferReceiver(const BtTransferReceiver&) = delete;
    BtTransferReceiver& operator=(const BtTransferReceiver&) = delete;

    bool start();
    void stop();
    bool is_running() const { return m_running.load(); }

    void set_on_receive_complete(ReceiveCompleteCallback callback);
    void set_on_receive_start(ReceiveStartCallback callback);
    void set_save_directory(const std::string& dir) { m_save_dir = dir; }

private:
    uint8_t     m_channel;              // RFCOMM 传输通道
    std::string m_save_dir;             // 文件保存目录
    SOCKET_FD   m_listen_socket;        // 监听 Socket
    std::atomic<bool> m_running;        // 运行状态
    std::thread m_accept_thread;        // 接受连接线程
    std::vector<std::thread> m_handler_threads;  // 处理线程
    std::mutex m_handler_mutex;

    ReceiveCompleteCallback m_complete_cb;
    ReceiveStartCallback    m_start_cb;

    /**
     * @brief 创建 RFCOMM 监听 Socket
     */
    bool create_listen_socket();

    /**
     * @brief 接受连接线程主函数
     */
    void accept_loop();

    /**
     * @brief 处理单个接收任务
     * @param client_sock  已建立的 RFCOMM 连接
     * @param sender_addr  发送方蓝牙地址
     */
    void handle_receive(SOCKET_FD client_sock, const std::string& sender_addr);

    /**
     * @brief 接收文件头信息 (JSON, 'J' 标记)
     */
    bool recv_file_header(SOCKET_FD sock, FileMeta& meta);

    /**
     * @brief 接收单个分片 (二进制, 'C' 标记)
     */
    bool recv_chunk(SOCKET_FD sock, Chunk& chunk);

    /**
     * @brief 发送 ACK (JSON, 'J' 标记)
     */
    bool send_ack(SOCKET_FD sock, const std::string& file_id,
                  uint32_t max_contiguous_chunk);
};
