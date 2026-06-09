#pragma once

// ============================================================
// 公共数据类型定义
// 功能: 定义整个项目中使用的核心数据结构
// ============================================================

#include <string>
#include <cstdint>
#include <chrono>
#include <vector>

// ----------------------------------------------------------
// 设备信息: 表示局域网中的一台在线设备
// ----------------------------------------------------------
struct DeviceInfo {
    std::string id;             // 设备唯一标识 (UUID格式, 如 "550e8400-e29b-41d4-a716-446655440000")
    std::string name;           // 设备显示名称 (用户自定义)
    std::string ip;             // 设备IPv4地址 (如 "192.168.1.100")
    uint16_t    port;           // 设备信令端口 (默认 8889)

    using TimePoint = std::chrono::steady_clock::time_point;
    TimePoint last_seen;        // 最后收到该设备广播的时间 (用于心跳超时检测)
    TimePoint first_seen;       // 首次发现该设备的时间
    bool      manual = false;   // 是否为手动添加 (手动添加的设备不会因超时被清理)
};

// ----------------------------------------------------------
// 文件元数据: 描述待传输文件的关键信息
// ----------------------------------------------------------
struct FileMeta {
    std::string file_id;        // 传输任务的唯一ID (UUID格式)
    std::string filename;       // 原始文件名 (含扩展名, 如 "论文.pdf")
    uint64_t    file_size;      // 文件总大小 (字节)
    uint32_t    chunk_size;     // 每个分片的大小 (字节, 默认 65536 = 64KB)
    uint32_t    total_chunks;   // 总分片数 = ceil(file_size / chunk_size)
    std::string compression;    // 压缩算法: "zlib" / "" (不压缩)
};

// ----------------------------------------------------------
// 传输状态枚举: 表示一个传输任务的当前状态
// ----------------------------------------------------------
enum class TransferState {
    IDLE,           // 空闲: 任务已创建但尚未开始
    NEGOTIATING,    // 协商中: 正在与目标设备协商文件传输参数
    TRANSFERRING,   // 传输中: 文件数据正在传输
    PAUSED,         // 已暂停: 传输被手动暂停 (可恢复)
    COMPLETED,      // 已完成: 文件传输成功, 校验通过
    FAILED,         // 失败: 传输过程中出现不可恢复的错误
    CANCELLED       // 已取消: 传输被用户取消
};

// ----------------------------------------------------------
// 传输任务: 表示一次完整的文件传输任务
// ----------------------------------------------------------
struct TransferTask {
    FileMeta           meta;              // 文件元数据
    DeviceInfo         target;            // 目标设备信息
    TransferState      state;             // 当前传输状态
    uint32_t           progress_chunk;    // 已完成的分片编号 (已确认的最大连续块号)
    uint64_t           bytes_sent;        // 已发送的字节总数
    double             speed;             // 当前传输速度 (字节/秒)
    uint32_t           window_size;       // 滑动窗口大小 (允许的未确认分片数)
    uint32_t           retry_count;       // 重传次数

    using TimePoint = std::chrono::steady_clock::time_point;
    TimePoint          start_time;        // 传输开始时间
    TimePoint          last_ack_time;     // 最后一次收到ACK的时间

    std::string        local_file_path;   // 本地文件路径
    bool               is_sender;         // true = 本机是发送方, false = 本机是接收方
};

// ----------------------------------------------------------
// 传输记录: 存储历史传输信息 (用于Web界面展示)
// ----------------------------------------------------------
struct TransferRecord {
    std::string file_id;           // 传输任务ID
    std::string filename;          // 文件名
    std::string device_name;       // 对方设备名称
    uint64_t    file_size;         // 文件大小
    bool        is_sender;         // true = 本机发送, false = 本机接收
    TransferState final_state;     // 最终状态
    int64_t     timestamp;         // 完成时间戳
};

// ----------------------------------------------------------
// 常量定义: 系统默认参数
// ----------------------------------------------------------
namespace Defaults {
    constexpr uint16_t    DISCOVERY_PORT   = 8888;   // 设备发现UDP端口
    constexpr uint16_t    SIGNALING_PORT   = 8889;   // 信令控制TCP端口
    constexpr uint16_t    TRANSFER_PORT    = 8890;   // 文件传输TCP端口
    constexpr uint16_t    HTTP_PORT        = 8891;   // Web界面HTTP端口
    constexpr uint32_t    BROADCAST_INTERVAL = 3;    // 广播间隔 (秒)
    constexpr uint32_t    DEVICE_TIMEOUT   = 10;      // 设备超时时间 (秒)
    constexpr uint32_t    CHUNK_SIZE       = 65536;   // 默认分片大小 (64KB)
    constexpr uint32_t    NUM_THREADS      = 4;       // 并行传输线程数
    constexpr uint32_t    WINDOW_SIZE      = 16;      // 滑动窗口大小 (BBR模式下备用)
    constexpr uint32_t    ACK_TIMEOUT_MS   = 3000;    // ACK超时重传时间 (毫秒)
    constexpr const char* MULTICAST_ADDR   = "239.255.255.250";  // 组播地址
}
