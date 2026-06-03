// ============================================================
// 文件分片读写 - 实现
// ============================================================

#include "transfer/file_io.h"
#include "common/utils.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>

FileChunkIO::FileChunkIO(const std::string& file_path, uint32_t chunk_size, bool is_sender)
    : m_file_path(file_path)
    , m_chunk_size(chunk_size)
    , m_file_size(0)
    , m_total_chunks(0)
    , m_is_sender(is_sender)
    , m_fd(-1)
{
    if (m_is_sender) {
        m_temp_path = m_file_path;  // 发送方直接使用原始文件
    } else {
        // 接收方使用临时文件
        m_temp_path = m_file_path + ".tmp";
    }
    init();
}

FileChunkIO::~FileChunkIO() {
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

bool FileChunkIO::init() {
    if (m_is_sender) {
        // 发送方: 读取文件大小
        m_file_size = Utils::get_file_size(m_file_path);
        if (m_file_size == 0) {
            return false;
        }
    } else {
        // 接收方: 检查是否有断点续传的临时文件
        m_file_size = Utils::get_file_size(m_temp_path);
    }

    // 计算总分片数
    m_total_chunks = static_cast<uint32_t>(
        (m_file_size + m_chunk_size - 1) / m_chunk_size
    );
    if (m_total_chunks == 0 && m_file_size > 0) {
        m_total_chunks = 1;
    }

    // 接收方: 初始化位图
    if (!m_is_sender) {
        m_received_bitmap.resize(m_total_chunks, false);

        // 读取已有临时文件的连续分片状态
        // 简化处理: 如果临时文件存在, 检查文件大小推断已接收的分片
        // 实际使用中, 这里可以读取一个 .meta 文件来精确知道哪些分片已接收
        // 为简单起见, 假设已有的临时文件是连续的 (从分片0开始)
        // 更精确的实现可以保存在 .meta 文件中

        // 打开临时文件 (用于随机写入分片)
        m_fd = open(m_temp_path.c_str(), O_WRONLY | O_CREAT, 0644);
        if (m_fd < 0) {
            std::cerr << "[文件IO] 无法打开临时文件: " << m_temp_path
                      << " (" << strerror(errno) << ")" << std::endl;
            return false;
        }

        // 如果临时文件已有数据, 标记已接收的分片
        uint64_t existing_size = Utils::get_file_size(m_temp_path);
        if (existing_size > 0) {
            uint32_t received_chunks = static_cast<uint32_t>(
                existing_size / m_chunk_size
            );
            for (uint32_t i = 0; i < received_chunks && i < m_total_chunks; ++i) {
                m_received_bitmap[i] = true;
            }
        }
    } else {
        // 发送方: 打开文件用于读取
        m_fd = open(m_file_path.c_str(), O_RDONLY);
        if (m_fd < 0) {
            std::cerr << "[文件IO] 无法打开文件: " << m_file_path
                      << " (" << strerror(errno) << ")" << std::endl;
            return false;
        }
    }

    return true;
}

// ----------------------------------------------------------
// 发送方: 读取分片
// ----------------------------------------------------------
bool FileChunkIO::read_chunk(uint32_t chunk_index, const std::string& file_id,
                              uint32_t total_chunks, Chunk& out_chunk) {
    if (!m_is_sender || m_fd < 0) {
        return false;
    }

    // 计算该分片在文件中的偏移和大小
    uint64_t offset = static_cast<uint64_t>(chunk_index) * m_chunk_size;
    uint32_t actual_size = m_chunk_size;
    if (chunk_index == m_total_chunks - 1) {
        // 最后一个分片可能小于 chunk_size
        actual_size = static_cast<uint32_t>(m_file_size - offset);
    }

    // 填充分片头部
    out_chunk.chunk_index  = chunk_index;
    out_chunk.total_chunks = total_chunks;
    out_chunk.file_id      = file_id;

    // 读取数据
    out_chunk.data.resize(actual_size);
    ssize_t n = pread(m_fd, out_chunk.data.data(), actual_size,
                      static_cast<off_t>(offset));
    if (n != static_cast<ssize_t>(actual_size)) {
        if (n == 0 && actual_size == 0) {
            // 空文件或空分片
            out_chunk.data_size = 0;
            return true;
        }
        return false;
    }

    out_chunk.data_size = actual_size;
    return true;
}

// ----------------------------------------------------------
// 接收方: 写入分片
// ----------------------------------------------------------
bool FileChunkIO::write_chunk(const Chunk& chunk) {
    if (m_is_sender || m_fd < 0) {
        return false;
    }

    if (chunk.chunk_index >= m_total_chunks) {
        std::cerr << "[文件IO] 分片编号超出范围: " << chunk.chunk_index
                  << " >= " << m_total_chunks << std::endl;
        return false;
    }

    // 计算写入偏移
    uint64_t offset = static_cast<uint64_t>(chunk.chunk_index) * m_chunk_size;

    // 写入数据
    ssize_t n = pwrite(m_fd, chunk.data.data(), chunk.data_size,
                       static_cast<off_t>(offset));
    if (n != static_cast<ssize_t>(chunk.data_size)) {
        std::cerr << "[文件IO] 写入失败: chunk=" << chunk.chunk_index
                  << " offset=" << offset << " size=" << chunk.data_size
                  << " n=" << n << std::endl;
        return false;
    }

    // 标记已接收
    m_received_bitmap[chunk.chunk_index] = true;

    return true;
}

uint32_t FileChunkIO::get_max_contiguous_chunk() const {
    uint32_t max_cont = 0;
    for (uint32_t i = 0; i < m_total_chunks; ++i) {
        if (m_received_bitmap[i]) {
            max_cont = i;
        } else {
            break;
        }
    }
    return max_cont > 0 ? max_cont : 0;
}

bool FileChunkIO::commit_received_file() {
    if (m_is_sender) return false;

    // 关闭文件句柄
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }

    // 重命名临时文件为正式文件
    if (::rename(m_temp_path.c_str(), m_file_path.c_str()) != 0) {
        std::cerr << "[文件IO] 重命名失败: " << m_temp_path
                  << " -> " << m_file_path << " (" << strerror(errno) << ")" << std::endl;
        return false;
    }

    std::cout << "[文件IO] 文件已提交: " << m_file_path << std::endl;
    return true;
}

void FileChunkIO::cancel_receive() {
    if (m_is_sender) return;

    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }

    // 删除临时文件
    ::unlink(m_temp_path.c_str());
    std::cout << "[文件IO] 临时文件已删除: " << m_temp_path << std::endl;
}
