#pragma once

// ============================================================
// 公共数据类型定义
// 功能: 定义整个项目中使用的核心数据结构
// 蓝牙版: 设备地址使用 BDADDR 而非 IPv4 地址
// ============================================================

#include <string>
#include <cstdint>
#include <chrono>
#include <vector>

// ----------------------------------------------------------
// 设备信息: 表示蓝牙范围内的一台在线设备
// ----------------------------------------------------------
struct DeviceInfo {
    std::string id;             // 设备唯一标识 (UUID格式, 如 "550e8400-e29b-41d4-a716-446655440000")
    std::string name;           // 设备显示名称 (蓝牙设备名)
    std::string addr;           // 蓝牙设备地址 (如 "AA:BB:CC:DD:EE:FF")
    uint16_t    port;           // 信令 RFCOMM 通道号 (默认 1)

    using TimePoint = std::chrono::steady_clock::time_point;
    TimePoint last_seen;        // 最后收到该设备信号的时间 (用于心跳超时检测)
    TimePoint first_seen;       // 首次发现该设备的时间
    TimePoint last_probed;      // 最后一次 RFCOMM 探活时间
    bool      manual = false;   // 是否为手动添加 (手动添加的设备不会因超时被清理)
    bool      connected = false;// 是否已通过 RFCOMM 握手确认连接 (绿灯/灰灯区分)
};

// ----------------------------------------------------------
// 文件元数据: 描述待传输文件的关键信息
// ----------------------------------------------------------
struct FileMeta {
    std::string file_id;        // 传输任务的唯一ID (UUID格式)
    std::string filename;       // 原始文件名 (含扩展名, 如 "论文.pdf")
    uint64_t    file_size;      // 文件总大小 (字节)
    std::string checksum;       // 文件MD5校验值 (用于传输后完整性验证)
    uint32_t    chunk_size;     // 每个分片的大小 (字节, 默认 65536 = 64KB)
    uint32_t    total_chunks;   // 总分片数 = ceil(file_size / chunk_size)
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
// 常量定义: 系统默认参数 (蓝牙版)
// ----------------------------------------------------------
namespace Defaults {
    constexpr uint8_t    SIGNALING_CHANNEL = 1;     // 信令控制 RFCOMM 通道
    constexpr uint8_t    TRANSFER_CHANNEL  = 2;     // 文件传输 RFCOMM 通道
    constexpr uint16_t   HTTP_PORT         = 8891;  // Web界面 HTTP 端口 (本机 localhost)
    constexpr uint32_t   DEVICE_TIMEOUT    = 15;    // 设备超时时间 (秒, 蓝牙扫描间隔大)
    constexpr uint32_t   CHUNK_SIZE        = 65536; // 默认分片大小 (64KB)
    constexpr uint32_t   WINDOW_SIZE       = 16;    // 滑动窗口大小 (可同时发送多少个未确认分片)
    constexpr uint32_t   ACK_TIMEOUT_MS    = 3000;  // ACK超时重传时间 (毫秒)
}
