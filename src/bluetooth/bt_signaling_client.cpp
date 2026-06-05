// ============================================================
// 蓝牙RFCOMM信令客户端 - 实现
// ============================================================

#include "bluetooth/bt_signaling_client.h"
#include "bluetooth/bt_utils.h"
#include "common/protocol.h"
#include "common/utils.h"
#include <iostream>
#include <cstring>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

BtSignalingClient::BtSignalingClient() {}
BtSignalingClient::~BtSignalingClient() {}

// ============================================================
// 发送请求 (短连接模式)
// ============================================================

bool BtSignalingClient::send_request(const std::string& target_addr,
                                      uint8_t channel,
                                      const json& request,
                                      json& response,
                                      uint32_t timeout_ms) {
    // 1. 连接到目标蓝牙设备
    SOCKET_FD sock = BtUtils::rfcomm_connect(target_addr, channel, timeout_ms);
    if (sock == INVALID_SOCKET_FD) {
        std::cerr << "[蓝牙信令客户端] 连接失败: " << target_addr
                  << " (通道 " << (int)channel << ")" << std::endl;
        return false;
    }

    // 2. 发送请求消息 (JSON + 长度前缀, 与 TCP 完全相同的格式)
    if (!Protocol::send_json_message(sock, request)) {
        std::cerr << "[蓝牙信令客户端] 发送请求失败" << std::endl;
        CLOSE_SOCKET(sock);
        return false;
    }

    std::cout << "[蓝牙信令客户端] 请求已发送: "
              << request.value("type", "") << " → "
              << target_addr << std::endl;

    // 3. 接收响应消息
    if (!Protocol::recv_json_message(sock, response)) {
        std::cerr << "[蓝牙信令客户端] 接收响应失败" << std::endl;
        CLOSE_SOCKET(sock);
        return false;
    }

    std::cout << "[蓝牙信令客户端] 收到响应: "
              << response.value("type", "") << " ("
              << response.value("status", "N/A") << ")" << std::endl;

    // 4. 关闭连接
    CLOSE_SOCKET(sock);
    return true;
}

// ============================================================
// 连通性测试 + DEVICE_HELLO 握手 (互相发现)
// ============================================================

bool BtSignalingClient::test_connect(const std::string& target_addr,
                                      uint8_t channel,
                                      const std::string& my_id,
                                      const std::string& my_name,
                                      const std::string& my_addr,
                                      uint8_t my_channel,
                                      uint32_t timeout_ms) {
    // 1. 非阻塞连接
    SOCKET_FD sock = BtUtils::rfcomm_connect(target_addr, channel, timeout_ms);
    if (sock == INVALID_SOCKET_FD) return false;

    // 2. 发送 DEVICE_HELLO 消息 (让对方发现本机)
    // 使用现有的协议函数, BDADDR 暂存于 ip 字段
    json hello = Protocol::build_device_hello(my_id, my_name, my_addr, my_channel);
    Protocol::send_json_message(sock, hello);

    // 3. 优雅关闭确保数据被对方收到
    shutdown(sock, SHUT_WR);
    char dummy[64];
    while (recv(sock, dummy, sizeof(dummy), 0) > 0) {}

    CLOSE_SOCKET(sock);
    return true;
}
