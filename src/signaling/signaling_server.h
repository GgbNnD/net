#pragma once

// ============================================================
// 信令服务端 (TCP)
// 功能: 监听信令端口, 接受其他设备的连接请求
//       接收并处理 FILE_REQUEST、PAUSE、CANCEL 等消息
// ============================================================

#include "common/types.h"
#include "common/platform.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ----------------------------------------------------------
// 响应发送器类型: 接收方用于发送响应消息的函数
// 参数: response_json - 要发送的响应消息
// 返回: 是否发送成功
// ----------------------------------------------------------
using ResponseSender = std::function<bool(const json& response)>;

// ----------------------------------------------------------
// 文件请求回调: 收到 FILE_REQUEST 时触发
// 参数: request  - 文件请求消息
//       sender   - 发送方IP地址
//       reply    - 响应发送器 (调用此函数发送 ACCEPT/REJECT)
// ----------------------------------------------------------
using FileRequestCallback = std::function<void(const json& request,
                                                const std::string& sender_ip,
                                                ResponseSender reply)>;

// ----------------------------------------------------------
// 控制消息回调: 收到 PAUSE/CANCEL/RESUME 等消息时触发
// 参数: msg       - 控制消息JSON
//       sender_ip - 发送方IP地址
// ----------------------------------------------------------
using ControlMsgCallback = std::function<void(const json& msg,
                                               const std::string& sender_ip)>;

// ----------------------------------------------------------
// 设备握手回调: 收到 DEVICE_HELLO 时触发 (TCP探活互相发现)
// ----------------------------------------------------------
using DeviceHelloCallback = std::function<void(const json& hello, const std::string& sender_ip)>;

/**
 * @class SignalingServer
 * @brief TCP信令服务端: 接收并处理其他设备发来的控制消息
 *
 * 工作流程:
 * 1. 创建TCP监听Socket, 绑定到信令端口 (8889)
 * 2. 启动接受线程: 循环 accept() 新连接
 * 3. 每个连接启动独立处理线程:
 *    - 收到 JSON 消息 (带长度前缀)
 *    - 根据消息类型分发到相应回调
 *    - FILE_REQUEST: 通过回调让上层决定 ACCEPT/REJECT
 *    - CANCEL/PAUSE/RESUME: 通知上层
 *    - 处理完成后关闭连接
 */
class SignalingServer {
public:
    SignalingServer(uint16_t port = Defaults::SIGNALING_PORT);
    ~SignalingServer();

    // ---- 禁止拷贝 ----
    SignalingServer(const SignalingServer&) = delete;
    SignalingServer& operator=(const SignalingServer&) = delete;

    /**
     * @brief 启动信令服务端
     * @return 是否启动成功
     */
    bool start();

    /**
     * @brief 停止信令服务端
     */
    void stop();

    /**
     * @brief 是否正在运行
     */
    bool is_running() const { return m_running.load(); }

    /**
     * @brief 获取监听端口
     */
    uint16_t get_port() const { return m_port; }

    /**
     * @brief 设置文件传输请求回调
     */
    void set_on_file_request(FileRequestCallback callback);

    /**
     * @brief 设置控制消息回调
     */
    void set_on_control_message(ControlMsgCallback callback);

    /**
     * @brief 设置 TCP 握手回调 (收到 DEVICE_HELLO 时触发)
     */
    void set_on_device_hello(DeviceHelloCallback callback);

    void set_ecdh_public_key(const std::string& key) { m_ecdh_public_key = key; }

private:
    uint16_t m_port;                    // 监听端口
    SOCKET_FD m_listen_socket;          // TCP监听Socket
    std::atomic<bool> m_running;        // 运行状态
    std::thread m_accept_thread;        // 接受连接线程

    // 跟踪所有处理线程 (避免 detach 导致僵尸线程)
    std::vector<std::thread> m_handler_threads;
    std::mutex m_handler_mutex;

    FileRequestCallback m_file_request_cb;     // 文件请求回调
    ControlMsgCallback  m_control_msg_cb;      // 控制消息回调
    DeviceHelloCallback m_device_hello_cb;     // TCP握手回调
    std::string m_ecdh_public_key;             // 本机 ECDH 公钥 (用于 HELLO_ACK)

    /**
     * @brief 接受连接线程主函数
     */
    void accept_loop();

    /**
     * @brief 处理单个客户端连接
     * @param client_sock 已建立的TCP连接socket
     * @param client_ip   客户端IP地址
     *
     * 处理流程:
     * 1. 接收客户端发来的JSON消息
     * 2. 根据消息类型分发到 m_file_request_cb 或 m_control_msg_cb
     * 3. 关闭连接
     */
    void handle_client(SOCKET_FD client_sock, const std::string& client_ip);

    /**
     * @brief 创建TCP监听Socket
     * @return 是否创建成功
     */
    bool create_listen_socket();
};
