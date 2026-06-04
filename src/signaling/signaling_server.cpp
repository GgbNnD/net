// ============================================================
// 信令服务端 - 实现
// ============================================================

#include "signaling/signaling_server.h"
#include "common/protocol.h"
#include "common/utils.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <algorithm>

// 等待连接的最大队列长度
constexpr int LISTEN_BACKLOG = 10;

// ============================================================
// 构造与析构
// ============================================================

SignalingServer::SignalingServer(uint16_t port)
    : m_port(port)
    , m_listen_socket(INVALID_SOCKET_FD)
    , m_running(false)
{
}

SignalingServer::~SignalingServer() {
    stop();
}

// ============================================================
// 公共接口
// ============================================================

bool SignalingServer::start() {
    if (m_running.load()) {
        return true;
    }

    if (!create_listen_socket()) {
        return false;
    }

    m_running = true;
    m_accept_thread = std::thread(&SignalingServer::accept_loop, this);

    std::cout << "[信令] 信令服务端启动 (端口 " << m_port << ")" << std::endl;
    return true;
}

void SignalingServer::stop() {
    if (!m_running.load()) {
        return;
    }

    std::cout << "[信令] 正在停止信令服务端..." << std::endl;

    m_running = false;

    // 关闭监听Socket (select() 会检测到并让 accept_loop 退出)
    if (m_listen_socket != INVALID_SOCKET_FD) {
        CLOSE_SOCKET(m_listen_socket);
        m_listen_socket = INVALID_SOCKET_FD;
    }

    if (m_accept_thread.joinable()) {
        m_accept_thread.join();
    }

    // 等待所有处理线程完成 (有5秒超时)
    {
        std::lock_guard<std::mutex> lock(m_handler_mutex);
        for (auto& t : m_handler_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        m_handler_threads.clear();
    }

    std::cout << "[信令] 信令服务端已停止" << std::endl;
}

void SignalingServer::set_on_file_request(FileRequestCallback callback) {
    m_file_request_cb = std::move(callback);
}

void SignalingServer::set_on_control_message(ControlMsgCallback callback) {
    m_control_msg_cb = std::move(callback);
}

void SignalingServer::set_on_device_hello(DeviceHelloCallback callback) {
    m_device_hello_cb = std::move(callback);
}

// ============================================================
// 内部方法
// ============================================================

bool SignalingServer::create_listen_socket() {
    // 1. 创建TCP Socket
    m_listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listen_socket == INVALID_SOCKET_FD) {
        std::cerr << "[信令] socket() 失败: "
                  << NetworkUtils::get_last_error_string() << std::endl;
        return false;
    }

    // 2. 设置地址重用
    NetworkUtils::set_reuse_addr(m_listen_socket);

    // 3. 绑定到信令端口
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(m_listen_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[信令] bind() 失败: "
                  << NetworkUtils::get_last_error_string() << std::endl;
        CLOSE_SOCKET(m_listen_socket);
        m_listen_socket = INVALID_SOCKET_FD;
        return false;
    }

    // 4. 开始监听
    if (listen(m_listen_socket, LISTEN_BACKLOG) < 0) {
        std::cerr << "[信令] listen() 失败: "
                  << NetworkUtils::get_last_error_string() << std::endl;
        CLOSE_SOCKET(m_listen_socket);
        m_listen_socket = INVALID_SOCKET_FD;
        return false;
    }

    return true;
}

// ----------------------------------------------------------
// 接受连接循环 (使用 select 避免 accept 永久阻塞)
// ----------------------------------------------------------
void SignalingServer::accept_loop() {
    std::cout << "[信令] 接受连接线程启动" << std::endl;

    while (m_running.load()) {
        // 使用 select() 检查监听Socket是否有新连接, 带200ms超时
        // 这样即使 close() 不能立刻唤醒 accept(), 也会在超时后退出
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(m_listen_socket, &read_fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;  // 200ms

        int sel_result = select((int)(m_listen_socket + 1),
                                &read_fds, nullptr, nullptr, &tv);

        if (sel_result < 0) {
            // select 错误 (监听Socket已被关闭), 退出循环
            break;
        }

        if (sel_result == 0) {
            // 超时, 循环回到 while (检查 m_running)
            continue;
        }

        // 有新连接, 调用 accept() 接受
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        SOCKET_FD client_sock = accept(m_listen_socket,
                                       (struct sockaddr*)&client_addr,
                                       &addr_len);

        if (client_sock == INVALID_SOCKET_FD) {
            // 监听Socket被关闭 (stop()调用), 退出
            break;
        }

        // 获取客户端IP
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        std::string client_ip(ip_str);

        std::cout << "[信令] 收到连接: " << client_ip
                  << ":" << ntohs(client_addr.sin_port) << std::endl;

        // 创建独立线程处理客户端请求
        std::thread handler([this, client_sock, client_ip]() {
            handle_client(client_sock, client_ip);
        });

        // 将线程加入管理列表 (只保留运行中的线程)
        {
            std::lock_guard<std::mutex> lock(m_handler_mutex);
            // 清理已完成的线程
            m_handler_threads.erase(
                std::remove_if(m_handler_threads.begin(), m_handler_threads.end(),
                    [](std::thread& t) {
                        if (t.joinable()) {
                            // 尚未完成, 保留
                            return false;
                        } else {
                            return true;  // 已完成, 移除
                        }
                    }),
                m_handler_threads.end()
            );
            m_handler_threads.push_back(std::move(handler));
        }
    }

    std::cout << "[信令] 接受连接线程退出" << std::endl;
}

// ----------------------------------------------------------
// 处理客户端连接
// ----------------------------------------------------------

void SignalingServer::handle_client(SOCKET_FD client_sock, const std::string& client_ip) {
    // 设置接收超时 (5秒), 避免客户端断开后线程永久阻塞
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    // 接收JSON消息 (TCP探活等连接可能不发数据, 静默关闭)
    json msg;
    if (!Protocol::recv_json_message(client_sock, msg)) {
        CLOSE_SOCKET(client_sock);
        return;
    }

    std::string type = msg.value("type", "");
    std::cout << "[信令] 收到消息类型: " << type << " (来自 " << client_ip << ")" << std::endl;

    // 根据消息类型分发
    if (type == MsgType::FILE_REQUEST) {
        // 文件传输请求: 创建响应发送器, 然后调用回调
        if (m_file_request_cb) {
            // 响应发送器: 捕获 client_sock 和 client_ip
            ResponseSender sender = [client_sock, client_ip](const json& response) -> bool {
                std::cout << "[信令] 发送响应给 " << client_ip
                          << ": " << response.value("status", "") << std::endl;
                return Protocol::send_json_message(client_sock, response);
            };

            m_file_request_cb(msg, client_ip, sender);
        } else {
            // 没有注册回调, 自动拒绝
            std::cout << "[信令] 无文件请求回调, 自动拒绝" << std::endl;
            json reject = Protocol::build_file_response(
                msg.value("file_id", ""), ResponseStatus::REJECT, 0, "未配置处理回调"
            );
            Protocol::send_json_message(client_sock, reject);
        }

    } else if (type == MsgType::DEVICE_HELLO) {
        // TCP探活握手: 通知上层添加发送方设备
        if (m_device_hello_cb) {
            m_device_hello_cb(msg, client_ip);
        }
    } else if (type == MsgType::TRANSFER_CANCEL ||
               type == MsgType::TRANSFER_PAUSE  ||
               type == MsgType::TRANSFER_RESUME ||
               type == MsgType::TRANSFER_DONE   ||
               type == MsgType::TRANSFER_ERROR) {
        // 控制消息: 通知上层
        if (m_control_msg_cb) {
            m_control_msg_cb(msg, client_ip);
        }
    } else {
        std::cerr << "[信令] 未知消息类型: " << type << std::endl;
    }

    // 关闭连接 (短连接模式)
    CLOSE_SOCKET(client_sock);
}
