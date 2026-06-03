#pragma once

// ============================================================
// 文件传输发送方 (TCP)
// 功能: 连接接收方, 使用滑动窗口协议发送文件分片
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

/**
 * @brief 传输进度信息
 */
struct TransferProgress {
    uint32_t total_chunks;         // 总分片数
    uint32_t sent_chunks;          // 已发送分片数
    uint32_t acked_chunks;         // 已确认分片数
    uint64_t bytes_sent;           // 已发送字节数
    double    speed;               // 当前速度 (bytes/s)
    uint64_t  elapsed_ms;          // 已用时间 (毫秒)
};

/**
 * @brief 传输进度回调类型
 */
using ProgressCallback = std::function<void(const TransferProgress& progress)>;

/**
 * @class TransferSender
 * @brief 文件传输发送方
 *
 * 发送流程:
 * 1. 连接到接收方的传输端口
 * 2. 发送文件头信息 (JSON)
 * 3. 使用滑动窗口逐片发送文件数据 (二进制)
 * 4. 接收ACK确认, 滑动窗口, 超时重传
 * 5. 发送完成后发送完成通知
 *
 * 滑动窗口参数:
 * - 窗口大小: 16个分片 (可同时有16个未确认分片)
 * - ACK超时: 3秒 (超时后重传)
 */
class TransferSender {
public:
    TransferSender();
    ~TransferSender();

    TransferSender(const TransferSender&) = delete;
    TransferSender& operator=(const TransferSender&) = delete;

    /**
     * @brief 发送文件到指定设备
     * @param target_ip     目标设备IP
     * @param target_port   传输端口 (默认 8890)
     * @param file_path     本地文件路径
     * @param file_id       传输任务ID
     * @param callback      进度回调 (可选)
     * @return 是否发送成功
     */
    bool send_file(const std::string& target_ip,
                   uint16_t target_port,
                   const std::string& file_path,
                   const std::string& file_id,
                   uint32_t window_size = Defaults::WINDOW_SIZE,
                   ProgressCallback callback = nullptr);

    /**
     * @brief 暂停传输
     *
     * 暂停后, send_file() 会阻塞直到 resume() 被调用
     */
    void pause();

    /**
     * @brief 恢复传输
     */
    void resume();

    /**
     * @brief 取消传输
     */
    void cancel();

    /**
     * @brief 是否正在传输
     */
    bool is_transferring() const { return m_transferring.load(); }

private:
    std::atomic<bool> m_paused;         // 暂停标志
    std::atomic<bool> m_cancelled;      // 取消标志
    std::atomic<bool> m_transferring;   // 传输中标志

    /**
     * @brief 发送文件头信息 (JSON格式)
     */
    bool send_file_header(SOCKET_FD sock, const FileMeta& meta);

    /**
     * @brief 发送所有分片 (滑动窗口)
     */
    bool send_chunks(SOCKET_FD sock, FileChunkIO& io, const FileMeta& meta,
                     uint32_t window_size, ProgressCallback callback);

    /**
     * @brief 发送单个分片
     */
    bool send_chunk(SOCKET_FD sock, const Chunk& chunk);

    /**
     * @brief 接收ACK消息 (非阻塞)
     * @return 收到ACK返回true, 无可用数据返回false
     */
    bool try_recv_ack(SOCKET_FD sock, nlohmann::json& ack);
};
