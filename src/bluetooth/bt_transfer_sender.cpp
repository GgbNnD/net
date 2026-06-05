// ============================================================
// 蓝牙RFCOMM文件传输发送方 - 实现
// ============================================================

#include "bluetooth/bt_transfer_sender.h"
#include "bluetooth/bt_utils.h"
#include "common/protocol.h"
#include "common/utils.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

BtTransferSender::BtTransferSender()
    : m_paused(false)
    , m_cancelled(false)
    , m_transferring(false)
{
}

BtTransferSender::~BtTransferSender() {
}

void BtTransferSender::pause() {
    m_paused = true;
    std::cout << "[蓝牙发送] 传输已暂停" << std::endl;
}

void BtTransferSender::resume() {
    m_paused = false;
    std::cout << "[蓝牙发送] 传输已恢复" << std::endl;
}

void BtTransferSender::cancel() {
    m_cancelled = true;
    std::cout << "[蓝牙发送] 传输已取消" << std::endl;
}

// ============================================================
// 主入口: 发送文件
// ============================================================

bool BtTransferSender::send_file(const std::string& target_addr,
                                  uint8_t channel,
                                  const std::string& file_path,
                                  const std::string& file_id,
                                  uint32_t window_size,
                                  ProgressCallback callback) {
    m_transferring = true;
    m_paused = false;
    m_cancelled = false;

    // 1. 打开文件并获取元信息
    FileChunkIO io(file_path, Defaults::CHUNK_SIZE, true);
    if (io.get_file_size() == 0) {
        std::cerr << "[蓝牙发送] 文件为空或无法打开: " << file_path << std::endl;
        m_transferring = false;
        return false;
    }

    FileMeta meta;
    meta.file_id      = file_id;
    meta.filename     = Utils::get_filename(file_path);
    meta.file_size    = io.get_file_size();
    meta.checksum     = Utils::md5_file(file_path);
    meta.chunk_size   = Defaults::CHUNK_SIZE;
    meta.total_chunks = io.get_total_chunks();

    std::cout << "[蓝牙发送] 准备发送: " << meta.filename
              << " (" << Utils::format_file_size(meta.file_size) << ")"
              << ", 分片数: " << meta.total_chunks
              << ", 校验: " << meta.checksum << std::endl;

    // 2. 连接到接收方 RFCOMM 传输通道
    SOCKET_FD sock = BtUtils::rfcomm_connect(target_addr, channel, 5000);
    if (sock == INVALID_SOCKET_FD) {
        std::cerr << "[蓝牙发送] 连接接收方失败: " << target_addr << std::endl;
        m_transferring = false;
        return false;
    }

    // 设置发送缓冲区 (蓝牙 RFCOMM 默认较小, 适当增大)
    int send_buf = 128 * 1024;  // 128KB
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
               (const char*)&send_buf, sizeof(send_buf));

    // 设置接收超时 (用于 try_recv_ack)
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;  // 500ms
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&tv, sizeof(tv));

    std::cout << "[蓝牙发送] 已连接到接收方: " << target_addr
              << " (通道 " << (int)channel << ")" << std::endl;

    // 3. 发送文件头
    if (!send_file_header(sock, meta)) {
        std::cerr << "[蓝牙发送] 发送文件头失败" << std::endl;
        CLOSE_SOCKET(sock);
        m_transferring = false;
        return false;
    }

    // 4. 发送文件数据分片 (滑动窗口)
    bool success = send_chunks(sock, io, meta, window_size, callback);

    // 5. 关闭连接
    CLOSE_SOCKET(sock);
    m_transferring = false;

    if (success) {
        std::cout << "[蓝牙发送] 文件发送成功: " << meta.filename << std::endl;
    } else {
        std::cerr << "[蓝牙发送] 文件发送失败或已取消" << std::endl;
    }

    return success;
}

// ============================================================
// 发送文件头 (JSON + 长度前缀 + 'J' 标记)
// ============================================================

bool BtTransferSender::send_file_header(SOCKET_FD sock, const FileMeta& meta) {
    json header;
    header["type"]         = "FILE_HEADER";
    header["file_id"]      = meta.file_id;
    header["filename"]     = meta.filename;
    header["file_size"]    = meta.file_size;
    header["checksum"]     = meta.checksum;
    header["chunk_size"]   = meta.chunk_size;
    header["total_chunks"] = meta.total_chunks;

    // 发送类型标记 'J' (JSON 消息)
    char type_marker = 'J';
    if (SOCK_SEND(sock, &type_marker, 1, 0) != 1) return false;

    return Protocol::send_json_message(sock, header);
}

// ============================================================
// 发送所有分片 (滑动窗口算法)
// ============================================================

bool BtTransferSender::send_chunks(SOCKET_FD sock, FileChunkIO& io,
                                    const FileMeta& meta,
                                    uint32_t window_size,
                                    ProgressCallback callback) {
    uint32_t next_chunk = 0;
    uint32_t base = 0;
    uint32_t total = meta.total_chunks;
    uint64_t bytes_sent = 0;
    auto start_time = std::chrono::steady_clock::now();

    int no_ack_cycles = 0;

    while (base < total && !m_cancelled.load()) {
        // 处理暂停
        while (m_paused.load() && !m_cancelled.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (m_cancelled.load()) break;

        // ---- 在窗口范围内发送多个分片 ----
        while (next_chunk < base + window_size && next_chunk < total) {
            Chunk chunk;
            if (!io.read_chunk(next_chunk, meta.file_id, total, chunk)) {
                return false;
            }
            if (!send_chunk(sock, chunk)) {
                return false;
            }
            bytes_sent += chunk.data_size;
            ++next_chunk;

            // 进度回调
            if (callback) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - start_time).count();
                double speed = elapsed > 0 ? (bytes_sent * 1000.0 / elapsed) : 0;
                TransferProgress progress;
                progress.total_chunks = total;
                progress.sent_chunks  = next_chunk;
                progress.acked_chunks = base;
                progress.bytes_sent   = bytes_sent;
                progress.speed        = speed;
                progress.elapsed_ms   = elapsed;
                callback(progress);
            }
        }

        // 短暂等待让接收方处理 (蓝牙延迟比 TCP 大)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // ---- 接收 ACK, 滑动窗口 ----
        json ack;
        if (try_recv_ack(sock, ack)) {
            if (ack.value("type", "") == MsgType::CHUNK_ACK) {
                uint32_t max_cont = ack.value("max_contiguous_chunk", uint32_t(0));
                if (max_cont + 1 > base) {
                    base = max_cont + 1;
                    no_ack_cycles = 0;
                }
            }
        } else {
            ++no_ack_cycles;
        }

        if (next_chunk >= total && base >= total) {
            break;
        }

        // 超时保护: WiFi 为 5 cycles, 蓝牙延迟更大, 放宽到 15 cycles (15 * 0.5s = 7.5s)
        if (next_chunk >= total && no_ack_cycles > 15) {
            std::cout << "[蓝牙发送] ACK 超时, 强制完成 (sent=" << next_chunk
                      << " acked=" << base << " total=" << total << ")" << std::endl;
            break;
        }
    }

    if (m_cancelled.load()) return false;

    std::cout << "[蓝牙发送] 全部分片已发送: " << total << " 片" << std::endl;
    return true;
}

// ============================================================
// 发送单个分片 (二进制 + 'C' 标记)
// ============================================================

bool BtTransferSender::send_chunk(SOCKET_FD sock, const Chunk& chunk) {
    std::vector<uint8_t> data = chunk.serialize();

    // 发送类型标记 'C' (Chunk 二进制数据)
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

// ============================================================
// 接收 ACK (带 SO_RCVTIMEO 超时)
// ============================================================

bool BtTransferSender::try_recv_ack(SOCKET_FD sock, nlohmann::json& ack) {
    char marker;
    int n = SOCK_RECV(sock, &marker, 1, 0);
    if (n != 1) return false;
    if (marker != 'J') return false;
    return Protocol::recv_json_message(sock, ack);
}
