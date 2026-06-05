// ============================================================
// 蓝牙RFCOMM信令服务端 - 实现
// ============================================================

#include "bluetooth/bt_signaling_server.h"
#include "bluetooth/bt_utils.h"
#include "common/protocol.h"
#include "common/utils.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <algorithm>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

constexpr int LISTEN_BACKLOG = 5;

// ============================================================
// 构造与析构
// ============================================================

BtSignalingServer::BtSignalingServer(uint8_t channel)
    : m_channel(channel)
    , m_listen_socket(INVALID_SOCKET_FD)
    , m_running(false)
{
}

BtSignalingServer::~BtSignalingServer() {
    stop();
}

// ============================================================
// 公共接口
// ============================================================

bool BtSignalingServer::start() {
    if (m_running.load()) return true;

    if (!create_listen_socket()) return false;

    m_running = true;
    m_accept_thread = std::thread(&BtSignalingServer::accept_loop, this);

    std::cout << "[蓝牙信令] 信令服务端启动 (通道 " << (int)m_channel << ")" << std::endl;
    return true;
}

void BtSignalingServer::stop() {
    if (!m_running.load()) return;

    std::cout << "[蓝牙信令] 正在停止信令服务端..." << std::endl;
    m_running = false;

    // 关闭监听 Socket 使 accept() / select() 返回
    if (m_listen_socket != INVALID_SOCKET_FD) {
        CLOSE_SOCKET(m_listen_socket);
        m_listen_socket = INVALID_SOCKET_FD;
    }

    if (m_accept_thread.joinable()) {
        m_accept_thread.join();
    }

    // 等待所有处理线程完成
    {
        std::lock_guard<std::mutex> lock(m_handler_mutex);
        for (auto& t : m_handler_threads) {
            if (t.joinable()) t.join();
        }
        m_handler_threads.clear();
    }

    std::cout << "[蓝牙信令] 信令服务端已停止" << std::endl;
}

void BtSignalingServer::set_on_file_request(FileRequestCallback callback) {
    m_file_request_cb = std::move(callback);
}

void BtSignalingServer::set_on_control_message(ControlMsgCallback callback) {
    m_control_msg_cb = std::move(callback);
}

void BtSignalingServer::set_on_device_hello(DeviceHelloCallback callback) {
    m_device_hello_cb = std::move(callback);
}

// ============================================================
// 内部方法
// ============================================================

bool BtSignalingServer::create_listen_socket() {
    m_listen_socket = BtUtils::create_rfcomm_server(m_channel, LISTEN_BACKLOG);
    return m_listen_socket != INVALID_SOCKET_FD;
}

void BtSignalingServer::accept_loop() {
    std::cout << "[蓝牙信令] 接受连接线程启动" << std::endl;

    while (m_running.load()) {
        // 使用 select() 避免 accept() 永久阻塞
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(m_listen_socket, &read_fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;  // 200ms

        int sel = select((int)(m_listen_socket + 1),
                         &read_fds, nullptr, nullptr, &tv);

        if (sel < 0) break;       // Socket 被关闭
        if (sel == 0) continue;   // 超时, 重新检查 m_running

        // 接受新连接
        struct sockaddr_rc client_addr;
        socklen_t addr_len = sizeof(client_addr);

        SOCKET_FD client_sock = accept(m_listen_socket,
                                        (struct sockaddr*)&client_addr,
                                        &addr_len);

        if (client_sock == INVALID_SOCKET_FD) break;

        // 获取客户端蓝牙地址
        std::string client_bdaddr = BtUtils::bdaddr_to_str(client_addr.rc_bdaddr);

        std::cout << "[蓝牙信令] 收到连接: " << client_bdaddr << std::endl;

        // 创建处理线程
        std::thread handler([this, client_sock, client_bdaddr]() {
            handle_client(client_sock, client_bdaddr);
        });

        // 管理处理线程
        {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            // 清理已完成线程
            m_handler_threads.erase(
                std::remove_if(m_handler_threads.begin(), m_handler_threads.end(),
                    [](std::thread& t) { return !t.joinable(); }),
                m_handler_threads.end()
            );
            m_handler_threads.push_back(std::move(handler));
        }
    }

    std::cout << "[蓝牙信令] 接受连接线程退出" << std::endl;
}

void BtSignalingServer::handle_client(SOCKET_FD client_sock,
                                       const std::string& client_addr) {
    // 设置接收超时 (5秒)
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO,
               (const char*)&tv, sizeof(tv));

    // 接收 JSON 消息
    json msg;
    if (!Protocol::recv_json_message(client_sock, msg)) {
        CLOSE_SOCKET(client_sock);
        return;
    }

    std::string type = msg.value("type", "");
    std::cout << "[蓝牙信令] 收到消息类型: " << type
              << " (来自 " << client_addr << ")" << std::endl;

    // 根据消息类型分发
    if (type == MsgType::FILE_REQUEST) {
        if (m_file_request_cb) {
            // 响应发送器: 捕获 client_sock
            ResponseSender sender = [client_sock, client_addr](const json& response) -> bool {
                std::cout << "[蓝牙信令] 发送响应给 " << client_addr
                          << ": " << response.value("status", "") << std::endl;
                return Protocol::send_json_message(client_sock, response);
            };
            m_file_request_cb(msg, client_addr, sender);
        } else {
            // 无回调, 自动拒绝
            std::cout << "[蓝牙信令] 无文件请求回调, 自动拒绝" << std::endl;
            json reject = Protocol::build_file_response(
                msg.value("file_id", ""), ResponseStatus::REJECT, 0, "未配置处理回调");
            Protocol::send_json_message(client_sock, reject);
        }
    }
    else if (type == MsgType::DEVICE_HELLO) {
        if (m_device_hello_cb) {
            m_device_hello_cb(msg, client_addr);
        }
    }
    else if (type == MsgType::TEXT_MESSAGE) {
        if (m_control_msg_cb) {
            m_control_msg_cb(msg, client_addr);
        }
    }
    else if (type == MsgType::TRANSFER_CANCEL ||
             type == MsgType::TRANSFER_PAUSE  ||
             type == MsgType::TRANSFER_RESUME ||
             type == MsgType::TRANSFER_DONE   ||
             type == MsgType::TRANSFER_ERROR) {
        if (m_control_msg_cb) {
            m_control_msg_cb(msg, client_addr);
        }
    }
    else {
        std::cerr << "[蓝牙信令] 未知消息类型: " << type << std::endl;
    }

    // 关闭连接 (短连接模式)
    CLOSE_SOCKET(client_sock);
}
