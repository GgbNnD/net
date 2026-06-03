#pragma once

// ============================================================
// 文件I/O操作 (分片读写)
// 功能: 将文件按固定大小分片读取和写入
// ============================================================

#include "transfer/chunk.h"
#include "common/types.h"
#include <string>
#include <vector>
#include <cstdint>

/**
 * @class FileChunkIO
 * @brief 文件分片读写操作
 *
 * 功能:
 * - read_chunk: 从文件中读取指定编号的分片
 * - write_chunk: 将分片数据写入临时文件
 * - 支持断点续传: get_received_chunks() 查询已写入的连续分片数
 * - 临时文件命名: "原文件名.tmp" (完成后重命名为原文件名)
 */
class FileChunkIO {
public:
    /**
     * @brief 构造函数
     * @param file_path 目标文件路径 (接收时传入目标路径, 发送时传入源文件路径)
     * @param chunk_size 分片大小 (默认 64KB)
     * @param is_sender true=发送方(读取), false=接收方(写入)
     */
    FileChunkIO(const std::string& file_path, uint32_t chunk_size = Defaults::CHUNK_SIZE,
                bool is_sender = true);

    ~FileChunkIO();

    // ---- 发送方接口: 读取分片 ----

    /**
     * @brief 读取指定编号的文件分片
     * @param chunk_index 分片编号 (0-based)
     * @param file_id     传输任务ID
     * @param total_chunks 总分片数
     * @param out_chunk   输出: 读取到的分片
     * @return 是否读取成功 (false 表示文件结束或读取错误)
     */
    bool read_chunk(uint32_t chunk_index, const std::string& file_id,
                    uint32_t total_chunks, Chunk& out_chunk);

    // ---- 接收方接口: 写入分片 ----

    /**
     * @brief 将接收到的分片写入临时文件
     * @param chunk 接收到的分片
     * @return 是否写入成功
     */
    bool write_chunk(const Chunk& chunk);

    /**
     * @brief 获取已接收的最大连续分片编号
     * @return 最大连续分片编号 (-1 表示没有任何分片)
     *
     * 用于断点续传: 如果已收到0~41号分片, 返回41
     *               如果有缺失(如0~40, 42~50), 返回40
     */
    uint32_t get_max_contiguous_chunk() const;

    /**
     * @brief 提交接收完成的文件 (重命名 .tmp 文件为正式文件)
     * @return 是否成功
     */
    bool commit_received_file();

    /**
     * @brief 取消接收 (删除临时文件)
     */
    void cancel_receive();

    /**
     * @brief 获取文件总大小
     */
    uint64_t get_file_size() const { return m_file_size; }

    /**
     * @brief 获取总分片数
     */
    uint32_t get_total_chunks() const { return m_total_chunks; }

    /**
     * @brief 获取分片大小
     */
    uint32_t get_chunk_size() const { return m_chunk_size; }

private:
    std::string m_file_path;       // 文件路径
    std::string m_temp_path;       // 临时文件路径 (接收方使用)
    uint32_t    m_chunk_size;      // 分片大小
    uint64_t    m_file_size;       // 文件总大小
    uint32_t    m_total_chunks;    // 总分片数
    bool        m_is_sender;       // true=发送方, false=接收方

    // 接收方: 位图追踪已接收的分片
    std::vector<bool> m_received_bitmap;  // true表示该分片已接收

    // 接收方: 文件句柄
    int m_fd;  // POSIX file descriptor (用于随机写入)

    /**
     * @brief 初始化文件元信息
     */
    bool init();
};
