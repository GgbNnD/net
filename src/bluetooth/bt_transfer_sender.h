#pragma once

// ============================================================
// 蓝牙RFCOMM文件传输发送方
// 功能: 替代 TCP TransferSender, 通过 RFCOMM 通道 2
//       使用滑动窗口协议发送文件分片
// ============================================================

#include "common/types.h"
#include "common/platform.h"
#include "transfer/file_io.h"
#include "transfer/chunk.h"
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>
#include <nlohmann/json.hpp>

// ---- 进度回调 (与原 TransferSender 一致) ----
struct TransferProgress {
    uint32_t total_chunks;
    uint32_t sent_chunks;
    uint32_t acked_chunks;
    uint64_t bytes_sent;
    double   speed;
    uint64_t elapsed_ms;
};

using ProgressCallback = std::function<void(const TransferProgress& progress)>;

/**
 * @class BtTransferSender
 * @brief 蓝牙 RFCOMM 文件传输发送方
 *
 * 发送流程:
 * 1. 连接到接收方的 RFCOMM 传输通道 (默认 2)
 * 2. 发送文件头信息 (JSON + 长度前缀 + 'J' 标记)
 * 3. 使用滑动窗口逐片发送文件数据 (二进制 + 'C' 标记)
 * 4. 接收 ACK 确认, 滑动窗口, 超时重传
 *
 * 滑动窗口参数:
 * - 窗口大小: 默认为 16 个分片
 * - ACK 超时: 接收方通过 SO_RCVTIMEO 控制
 */
class BtTransferSender {
public:
    BtTransferSender();
    ~BtTransferSender();

    BtTransferSender(const BtTransferSender&) = delete;
    BtTransferSender& operator=(const BtTransferSender&) = delete;

    /**
     * @brief 发送文件到指定蓝牙设备
     * @param target_addr  目标蓝牙地址
     * @param channel      RFCOMM 传输通道 (默认 2)
     * @param file_path    本地文件路径
     * @param file_id      传输任务ID
     * @param window_size  滑动窗口大小
     * @param callback     进度回调 (可选)
     * @return 是否发送成功
     */
    bool send_file(const std::string& target_addr,
                   uint8_t channel,
                   const std::string& file_path,
                   const std::string& file_id,
                   uint32_t window_size = Defaults::WINDOW_SIZE,
                   ProgressCallback callback = nullptr);

    void pause();
    void resume();
    void cancel();
    bool is_transferring() const { return m_transferring.load(); }

private:
    std::atomic<bool> m_paused;
    std::atomic<bool> m_cancelled;
    std::atomic<bool> m_transferring;

    /**
     * @brief 发送文件头信息 (JSON, 带 'J' 类型标记)
     */
    bool send_file_header(SOCKET_FD sock, const FileMeta& meta);

    /**
     * @brief 发送所有分片 (滑动窗口算法)
     */
    bool send_chunks(SOCKET_FD sock, FileChunkIO& io, const FileMeta& meta,
                     uint32_t window_size, ProgressCallback callback);

    /**
     * @brief 发送单个分片 (二进制格式, 带 'C' 类型标记)
     */
    bool send_chunk(SOCKET_FD sock, const Chunk& chunk);

    /**
     * @brief 接收 ACK 消息 (非阻塞, 带 SO_RCVTIMEO 超时)
     */
    bool try_recv_ack(SOCKET_FD sock, nlohmann::json& ack);
};
