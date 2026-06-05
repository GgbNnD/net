#pragma once

// ============================================================
// 蓝牙RFCOMM信令客户端
// 功能: 替代 TCP SignalingClient, 通过 RFCOMM 向目标蓝牙设备
//       发送信令消息并接收响应
// ============================================================

#include "common/types.h"
#include "common/platform.h"
#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * @class BtSignalingClient
 * @brief 蓝牙 RFCOMM 信令客户端
 *
 * 使用方式 (同步):
 *   BtSignalingClient client;
 *   json response;
 *   if (client.send_request(target_addr, channel, request, response)) {
 *       // 处理 response
 *   }
 *
 * 特点:
 * - 短连接模式: 连接 → 发送请求 → 接收响应 → 断开
 * - 带超时机制 (非阻塞 connect + select)
 * - RFCOMM 通道比 TCP 端口更简单 (直接指定通道号)
 */
class BtSignalingClient {
public:
    BtSignalingClient();
    ~BtSignalingClient();

    BtSignalingClient(const BtSignalingClient&) = delete;
    BtSignalingClient& operator=(const BtSignalingClient&) = delete;

    /**
     * @brief 向目标蓝牙设备发送信令请求并接收响应
     * @param target_addr 目标蓝牙地址 "AA:BB:CC:DD:EE:FF"
     * @param channel     RFCOMM 通道号 (默认 1)
     * @param request     请求消息 (JSON)
     * @param response    输出: 响应消息
     * @param timeout_ms  超时时间 (毫秒, 默认 5000)
     * @return 成功返回 true
     */
    bool send_request(const std::string& target_addr,
                      uint8_t channel,
                      const json& request,
                      json& response,
                      uint32_t timeout_ms = 5000);

    /**
     * @brief 快速测试蓝牙连通性 + 发送 DEVICE_HELLO 握手 (互相发现)
     * @param target_addr 目标蓝牙地址
     * @param channel     目标 RFCOMM 通道
     * @param my_id       本机设备ID
     * @param my_name     本机设备名称
     * @param my_addr     本机蓝牙地址
     * @param my_channel  本机信令通道
     * @param timeout_ms  超时时间 (默认 1000ms)
     * @return 能握手成功返回 true
     *
     * 与原 SignalingClient::test_connect() 功能完全等价,
     * 只是用蓝牙地址和通道号替代了 IP 和端口
     */
    static bool test_connect(const std::string& target_addr,
                             uint8_t channel,
                             const std::string& my_id,
                             const std::string& my_name,
                             const std::string& my_addr,
                             uint8_t my_channel,
                             uint32_t timeout_ms = 1000);
};
