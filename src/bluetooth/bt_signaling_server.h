#pragma once

// ============================================================
// 蓝牙RFCOMM信令服务端
// 功能: 替代 TCP SignalingServer, 在 RFCOMM 通道上监听
//       接收其他蓝牙设备的控制消息 (FILE_REQUEST, DEVICE_HELLO 等)
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

// ---- 回调类型 (与 SignalingServer 完全兼容) ----
using ResponseSender     = std::function<bool(const json& response)>;
using FileRequestCallback = std::function<void(const json& request,
                                                const std::string& sender_addr,
                                                ResponseSender reply)>;
using ControlMsgCallback  = std::function<void(const json& msg,
                                                const std::string& sender_addr)>;
using DeviceHelloCallback = std::function<void(const json& hello,
                                                const std::string& sender_addr)>;

/**
 * @class BtSignalingServer
 * @brief 蓝牙 RFCOMM 信令服务端
 *
 * 工作流程:
 * 1. 创建 RFCOMM Socket, 绑定到通道 1
 * 2. 启动接受线程: 循环 accept() 新连接
 * 3. 每个连接启动独立处理线程:
 *    - 接收 JSON 消息 (带长度前缀, 与 TCP 相同)
 *    - 根据消息类型分发到回调
 *    - 处理完成后关闭连接 (短连接模式)
 *
 * 线程安全: 与原 SignalingServer 相同的模式
 */
class BtSignalingServer {
public:
    /**
     * @brief 构造函数
     * @param channel RFCOMM 通道号 (默认 1)
     */
    explicit BtSignalingServer(uint8_t channel = 1);

    ~BtSignalingServer();

    // ---- 禁止拷贝 ----
    BtSignalingServer(const BtSignalingServer&) = delete;
    BtSignalingServer& operator=(const BtSignalingServer&) = delete;

    bool start();
    void stop();
    bool is_running() const { return m_running.load(); }
    uint8_t get_channel() const { return m_channel; }

    void set_on_file_request(FileRequestCallback callback);
    void set_on_control_message(ControlMsgCallback callback);
    void set_on_device_hello(DeviceHelloCallback callback);

private:
    uint8_t    m_channel;              // RFCOMM 通道号
    SOCKET_FD  m_listen_socket;        // RFCOMM 监听 Socket
    std::atomic<bool> m_running;       // 运行状态
    std::thread m_accept_thread;       // 接受连接线程

    // 处理线程跟踪
    std::vector<std::thread> m_handler_threads;
    std::mutex m_handler_mutex;

    // 回调函数
    FileRequestCallback m_file_request_cb;
    ControlMsgCallback  m_control_msg_cb;
    DeviceHelloCallback m_device_hello_cb;

    /**
     * @brief 创建 RFCOMM 监听 Socket
     */
    bool create_listen_socket();

    /**
     * @brief 接受连接线程主函数
     */
    void accept_loop();

    /**
     * @brief 处理单个客户端连接
     * @param client_sock  已建立的 RFCOMM 连接
     * @param client_addr  客户端蓝牙地址
     */
    void handle_client(SOCKET_FD client_sock, const std::string& client_addr);
};
