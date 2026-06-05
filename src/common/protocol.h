#pragma once

// ============================================================
// 自定义应用层协议定义
// 功能: 定义设备发现、信令协商、文件传输三个阶段的消息格式
//       所有消息使用JSON格式, 便于调试和扩展
// ============================================================

#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "common/platform.h"

using json = nlohmann::json;

// ----------------------------------------------------------
// 消息类型常量
// 使用constexpr定义, 避免魔法字符串, 编译期检查
// ----------------------------------------------------------
namespace MsgType {
    // === 设备发现阶段 ===
    constexpr const char* DEVICE_BROADCAST = "DEVICE_BROADCAST";  // 设备上线广播
    constexpr const char* DEVICE_OFFLINE   = "DEVICE_OFFLINE";    // 设备离线通知
    constexpr const char* DEVICE_HELLO     = "DEVICE_HELLO";      // TCP探活握手 (互相发现)
    constexpr const char* TEXT_MESSAGE     = "TEXT_MESSAGE";      // 聊天文本消息

    // === 信令协商阶段 ===
    constexpr const char* FILE_REQUEST     = "FILE_REQUEST";      // 文件传输请求
    constexpr const char* FILE_RESPONSE    = "FILE_RESPONSE";     // 文件传输响应 (ACCEPT/REJECT)
    constexpr const char* TRANSFER_CANCEL  = "TRANSFER_CANCEL";   // 取消传输
    constexpr const char* TRANSFER_PAUSE   = "TRANSFER_PAUSE";    // 暂停传输
    constexpr const char* TRANSFER_RESUME  = "TRANSFER_RESUME";   // 恢复传输

    // === 文件传输阶段 ===
    constexpr const char* CHUNK_ACK        = "CHUNK_ACK";         // 分片确认 (接收->发送)
    constexpr const char* TRANSFER_DONE    = "TRANSFER_DONE";     // 传输完成通知
    constexpr const char* TRANSFER_ERROR   = "TRANSFER_ERROR";    // 传输错误通知
}

// ----------------------------------------------------------
// 响应状态常量
// ----------------------------------------------------------
namespace ResponseStatus {
    constexpr const char* ACCEPT = "ACCEPT";   // 接受传输
    constexpr const char* REJECT = "REJECT";   // 拒绝传输
    constexpr const char* PAUSE  = "PAUSE";    // 暂停传输
}

// ============================================================
// 协议消息构建函数
// 每个函数返回一个构造好的JSON对象, 可直接序列化发送
// ============================================================
namespace Protocol {

/**
 * @brief 构建设备广播消息 (蓝牙发现 / 手动连接推送)
 * @param device_id   设备唯一ID
 * @param device_name 设备显示名称
 * @param addr        设备蓝牙地址 (BDADDR)
 * @param channel     信令 RFCOMM 通道号
 * @param timestamp   当前时间戳 (毫秒)
 * @return JSON消息对象
 *
 * 消息格式:
 * {
 *   "type": "DEVICE_BROADCAST",
 *   "device_id": "uuid",
 *   "device_name": "MyDevice",
 *   "addr": "AA:BB:CC:DD:EE:FF",
 *   "channel": 1,
 *   "timestamp": 1234567890
 * }
 */
json build_device_broadcast(const std::string& device_id,
                             const std::string& device_name,
                             const std::string& addr,
                             uint16_t channel,
                             uint64_t timestamp);

/**
 * @brief 构建设备离线通知消息
 * @param device_id 设备唯一ID
 * @return JSON消息对象
 */
json build_device_offline(const std::string& device_id);

/**
 * @brief 构建 RFCOMM 探活握手消息 (互相发现)
 */
json build_device_hello(const std::string& device_id,
                         const std::string& device_name,
                         const std::string& addr,
                         uint16_t channel);

/**
 * @brief 构建文本聊天消息
 */
json build_text_message(const std::string& text);

/**
 * @brief 构建文件传输请求消息 (TCP信令通道)
 * @param file_id      传输任务ID
 * @param filename     文件名
 * @param file_size    文件大小 (字节)
 * @param checksum     文件MD5校验值
 * @param chunk_size   分片大小 (字节)
 * @param total_chunks 总分片数
 * @return JSON消息对象
 *
 * 消息格式:
 * {
 *   "type": "FILE_REQUEST",
 *   "file_id": "uuid",
 *   "filename": "论文.pdf",
 *   "file_size": 10485760,
 *   "checksum": "d41d8cd98f00b204e9800998ecf8427e",
 *   "chunk_size": 65536,
 *   "total_chunks": 160
 * }
 */
json build_file_request(const std::string& file_id,
                         const std::string& filename,
                         uint64_t file_size,
                         const std::string& checksum,
                         uint32_t chunk_size,
                         uint32_t total_chunks);

/**
 * @brief 构建文件传输响应消息 (TCP信令通道)
 * @param file_id           传输任务ID
 * @param status            响应状态: "ACCEPT" / "REJECT" / "PAUSE"
 * @param resume_from_chunk 断点续传起始分片编号 (ACCEPT时使用)
 * @param reason            拒绝原因 (REJECT时使用, 可选)
 * @return JSON消息对象
 *
 * 消息格式:
 * {
 *   "type": "FILE_RESPONSE",
 *   "file_id": "uuid",
 *   "status": "ACCEPT",
 *   "resume_from_chunk": 0,
 *   "reason": ""
 * }
 */
json build_file_response(const std::string& file_id,
                          const std::string& status,
                          uint32_t resume_from_chunk,
                          const std::string& reason = "");

/**
 * @brief 构建分片确认消息 (TCP数据通道)
 * @param file_id             传输任务ID
 * @param max_contiguous_chunk 已确认的最大连续块号
 * @return JSON消息对象
 *
 * 消息格式:
 * {
 *   "type": "CHUNK_ACK",
 *   "file_id": "uuid",
 *   "max_contiguous_chunk": 42
 * }
 */
json build_chunk_ack(const std::string& file_id,
                      uint32_t max_contiguous_chunk);

/**
 * @brief 构建控制类消息 (取消/暂停/恢复/完成/错误)
 * @param type    消息类型 (见 MsgType)
 * @param file_id 传输任务ID
 * @param extra   附加信息 (错误消息等, 可选)
 * @return JSON消息对象
 */
json build_control_message(const std::string& type,
                            const std::string& file_id,
                            const std::string& extra = "");

/**
 * @brief 发送JSON消息 (带长度前缀, 解决流式传输粘包问题)
 * @param sock socket描述符
 * @param msg  JSON消息对象
 * @return 是否发送成功
 *
 * 协议格式: [4字节消息长度(网络字节序)] + [JSON字符串]
 */
bool send_json_message(SOCKET_FD sock, const json& msg);

/**
 * @brief 接收JSON消息 (带长度前缀解析)
 * @param sock socket描述符
 * @param msg  输出: 解析后的JSON对象
 * @return 是否接收成功
 */
bool recv_json_message(SOCKET_FD sock, json& msg);

} // namespace Protocol
