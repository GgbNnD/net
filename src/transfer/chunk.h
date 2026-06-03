// ============================================================
// 文件分片数据结构
// ============================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

// 最大分片大小: 64KB + 头部信息
constexpr uint32_t MAX_CHUNK_DATA_SIZE = 65536;  // 64KB
constexpr uint32_t CHUNK_HEADER_SIZE   = 16;     // 分片头部大小 (字节)

/**
 * @brief 文件分片结构体 (网络传输格式)
 *
 * 分片头部格式 (16字节, 网络字节序):
 * ┌────────────────┬────────────────┬────────────────┬────────────────┐
 * │ chunk_index    │ total_chunks   │ data_size      │ file_id_len    │
 * │ (4字节 uint32) │ (4字节 uint32) │ (4字节 uint32) │ (4字节 uint32) │
 * ├────────────────┴────────────────┴────────────────┴────────────────┤
 * │ file_id (变长, SHA1/uuid格式, 通常36字节)                         │
 * ├──────────────────────────────────────────────────────────────────┤
 * │ data (变长, 最多 MAX_CHUNK_DATA_SIZE 字节)                       │
 * └──────────────────────────────────────────────────────────────────┘
 */
struct Chunk {
    uint32_t chunk_index;     // 分片编号 (从0开始)
    uint32_t total_chunks;    // 总分片数
    uint32_t data_size;       // 有效数据长度 (字节)
    std::string file_id;      // 传输任务ID
    std::vector<uint8_t> data; // 分片数据

    /**
     * @brief 序列化为网络传输格式
     * @return 序列化后的字节数组
     */
    std::vector<uint8_t> serialize() const {
        uint32_t file_id_len = static_cast<uint32_t>(file_id.size());
        uint32_t total_size = CHUNK_HEADER_SIZE + file_id_len + data_size;
        std::vector<uint8_t> buffer(total_size);

        // 写入头部 (网络字节序 = 大端)
        write_u32_be(buffer.data(), 0, chunk_index);
        write_u32_be(buffer.data(), 4, total_chunks);
        write_u32_be(buffer.data(), 8, data_size);
        write_u32_be(buffer.data(), 12, file_id_len);

        // 写入 file_id
        memcpy(buffer.data() + CHUNK_HEADER_SIZE, file_id.data(), file_id_len);

        // 写入数据
        if (data_size > 0) {
            memcpy(buffer.data() + CHUNK_HEADER_SIZE + file_id_len, data.data(), data_size);
        }

        return buffer;
    }

    /**
     * @brief 从网络传输格式反序列化
     * @param buffer 接收到的字节数据
     * @param length 数据长度
     * @return 是否解析成功
     */
    bool deserialize(const uint8_t* buffer, size_t length) {
        if (length < CHUNK_HEADER_SIZE) return false;

        chunk_index  = read_u32_be(buffer, 0);
        total_chunks = read_u32_be(buffer, 4);
        data_size    = read_u32_be(buffer, 8);
        uint32_t file_id_len = read_u32_be(buffer, 12);

        // 安全检查
        if (data_size > MAX_CHUNK_DATA_SIZE) return false;
        if (file_id_len > 256) return false;
        if (CHUNK_HEADER_SIZE + file_id_len + data_size > length) return false;

        // 读取 file_id
        file_id.assign(reinterpret_cast<const char*>(buffer + CHUNK_HEADER_SIZE), file_id_len);

        // 读取数据
        data.resize(data_size);
        if (data_size > 0) {
            memcpy(data.data(), buffer + CHUNK_HEADER_SIZE + file_id_len, data_size);
        }

        return true;
    }

private:
    static void write_u32_be(uint8_t* buf, size_t offset, uint32_t val) {
        buf[offset]     = (val >> 24) & 0xFF;
        buf[offset + 1] = (val >> 16) & 0xFF;
        buf[offset + 2] = (val >> 8)  & 0xFF;
        buf[offset + 3] = val & 0xFF;
    }

    static uint32_t read_u32_be(const uint8_t* buf, size_t offset) {
        return ((uint32_t)buf[offset] << 24) |
               ((uint32_t)buf[offset + 1] << 16) |
               ((uint32_t)buf[offset + 2] << 8) |
               (uint32_t)buf[offset + 3];
    }
};
