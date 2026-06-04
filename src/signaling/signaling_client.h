#pragma once

// ============================================================
// 信令客户端 (TCP)
// 功能: 向目标设备的信令端口发起连接,
//       发送请求消息并接收响应
// ============================================================

#include "common/types.h"
#include "common/platform.h"
#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * @class SignalingClient
 * @brief TCP信令客户端: 向目标设备发送信令消息并接收响应
 *
 * 使用方式 (同步):
 *   SignalingClient client;
 *   json response;
 *   if (client.send_request(target_ip, target_port, request, response)) {
 *       // 处理 response
 *   }
 *
 * 特点:
 * - 短连接模式: 连接 -> 发送请求 -> 接收响应 -> 断开
 * - 带超时机制, 避免长时间阻塞
 */
class SignalingClient {
public:
    SignalingClient();
    ~SignalingClient();

    // ---- 禁止拷贝 ----
    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    /**
     * @brief 向目标设备发送信令请求并接收响应
     * @param target_ip     目标设备IP地址
     * @param target_port   目标设备信令端口
     * @param request       请求消息 (JSON格式)
     * @param response      输出: 响应消息
     * @param timeout_ms    超时时间 (毫秒, 默认 5000)
     * @return 成功发送并收到响应返回 true
     *
     * 流程:
     * 1. 连接到 target_ip:target_port
     * 2. 发送请求 (带长度前缀的JSON)
     * 3. 等待并接收响应
     * 4. 关闭连接
     */
    bool send_request(const std::string& target_ip,
                      uint16_t target_port,
                      const json& request,
                      json& response,
                      uint32_t timeout_ms = 5000);

    /**
     * @brief 快速测试目标设备的TCP连通性 (不发送任何数据)
     * @param target_ip   目标IP
     * @param target_port 目标端口
     * @param timeout_ms  超时时间 (默认500ms)
     * @return 能建立TCP连接返回 true
     */
    static bool test_connect(const std::string& target_ip,
                             uint16_t target_port,
                             uint32_t timeout_ms = 500);

private:
    /**
     * @brief 创建TCP Socket并连接到目标
     * @param target_ip   目标IP
     * @param target_port 目标端口
     * @param timeout_ms  超时时间
     * @return 成功返回socket描述符, 失败返回 INVALID_SOCKET_FD
     */
    SOCKET_FD connect_to(const std::string& target_ip,
                         uint16_t target_port,
                         uint32_t timeout_ms);
};
