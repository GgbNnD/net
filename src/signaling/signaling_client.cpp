// ============================================================
// 信令客户端 - 实现
// ============================================================

#include "signaling/signaling_client.h"
#include "common/protocol.h"
#include "common/utils.h"
#include <iostream>
#include <cstring>

// ============================================================
// 构造与析构
// ============================================================

SignalingClient::SignalingClient() {
}

SignalingClient::~SignalingClient() {
}

/**
 * @brief TCP探活 + DEVICE_HELLO 握手 (双方互相发现)
 */
bool SignalingClient::test_connect(const std::string& target_ip,
                                     uint16_t target_port,
                                     const std::string& my_id,
                                     const std::string& my_name,
                                     const std::string& my_ip,
                                     uint16_t my_port,
                                     uint32_t timeout_ms) {
    SOCKET_FD sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET_FD) return false;

    NetworkUtils::set_nonblocking(sock);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(target_port);
    if (inet_pton(AF_INET, target_ip.c_str(), &addr.sin_addr) <= 0) {
        CLOSE_SOCKET(sock);
        return false;
    }

    int conn_result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (conn_result < 0) {
        int err = GET_SOCKET_ERROR();
        if (!NetworkUtils::is_would_block(err)) {
            CLOSE_SOCKET(sock);
            return false;
        }
    } else {
        // 立即连接成功, 发送 DEVICE_HELLO
        NetworkUtils::set_blocking(sock);
        json hello = Protocol::build_device_hello(my_id, my_name, my_ip, my_port);
        Protocol::send_json_message(sock, hello);
        // 优雅关闭确保数据被对方收到
        shutdown(sock, SHUT_WR);
        char dummy[64];
        while (recv(sock, dummy, sizeof(dummy), 0) > 0) {}
        CLOSE_SOCKET(sock);
        return true;
    }

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);

    struct timeval timeout;
    timeout.tv_sec  = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    int select_result = select((int)(sock + 1), nullptr, &write_fds, nullptr, &timeout);

    if (select_result <= 0) {
        CLOSE_SOCKET(sock);
        return false;
    }

    int so_error = 0;
    socklen_t so_len = sizeof(so_error);
    bool ok = (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &so_len) == 0 && so_error == 0);

    if (ok) {
        // 连接成功, 发送 DEVICE_HELLO 让对方发现本机
        NetworkUtils::set_blocking(sock);
        json hello = Protocol::build_device_hello(my_id, my_name, my_ip, my_port);
        Protocol::send_json_message(sock, hello);
        // 优雅关闭确保数据被对方收到
        shutdown(sock, SHUT_WR);
        char dummy[64];
        while (recv(sock, dummy, sizeof(dummy), 0) > 0) {}
    }

    CLOSE_SOCKET(sock);
    return ok;
}

// ============================================================
// 内部方法
// ============================================================

/**
 * @brief 向目标设备发送信令请求并接收响应
 *
 * 短连接模式:
 * 1. 连接到目标设备的信令端口
 * 2. 发送请求消息
 * 3. 接收响应消息
 * 4. 断开连接
 *
 * @param target_ip   目标设备IP地址
 * @param target_port 目标设备信令端口
 * @param request     请求消息
 * @param response    输出: 响应消息
 * @param timeout_ms  超时时间 (毫秒)
 * @return 是否成功
 */
bool SignalingClient::send_request(const std::string& target_ip,
                                    uint16_t target_port,
                                    const json& request,
                                    json& response,
                                    uint32_t timeout_ms) {
    // 1. 连接到目标设备
    SOCKET_FD sock = connect_to(target_ip, target_port, timeout_ms);
    if (sock == INVALID_SOCKET_FD) {
        std::cerr << "[信令客户端] 连接失败: " << target_ip << ":" << target_port << std::endl;
        return false;
    }

    // 2. 发送请求消息
    if (!Protocol::send_json_message(sock, request)) {
        std::cerr << "[信令客户端] 发送请求失败" << std::endl;
        CLOSE_SOCKET(sock);
        return false;
    }

    std::cout << "[信令客户端] 请求已发送: "
              << request.value("type", "") << " -> " << target_ip << ":" << target_port
              << std::endl;

    // 3. 接收响应消息
    if (!Protocol::recv_json_message(sock, response)) {
        std::cerr << "[信令客户端] 接收响应失败" << std::endl;
        CLOSE_SOCKET(sock);
        return false;
    }

    std::cout << "[信令客户端] 收到响应: "
              << response.value("type", "") << " ("
              << response.value("status", "N/A") << ")" << std::endl;

    // 4. 关闭连接
    CLOSE_SOCKET(sock);
    return true;
}

// ============================================================
// 内部方法: 带超时的TCP连接
// ============================================================

/**
 * @brief 创建TCP Socket并连接到目标设备
 *
 * 超时机制说明:
 * 标准的 connect() 默认是阻塞的, Linux上默认超时时间约 3 分钟,
 * 我们需要缩短这个超时时间来提升用户体验。
 *
 * 实现方法: 使用非阻塞Socket + select()
 * 1. 创建Socket, 设置为非阻塞
 * 2. 调用 connect() (非阻塞模式会立即返回, errno=EINPROGRESS)
 * 3. 使用 select() 或 poll() 等待指定的超时时间
 * 4. 连接成功后恢复为阻塞模式 (方便后续同步收发)
 */
SOCKET_FD SignalingClient::connect_to(const std::string& target_ip,
                                       uint16_t target_port,
                                       uint32_t timeout_ms) {
    // 1. 创建TCP Socket
    SOCKET_FD sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET_FD) {
        std::cerr << "[信令客户端] socket() 失败: "
                  << NetworkUtils::get_last_error_string() << std::endl;
        return INVALID_SOCKET_FD;
    }

    // 2. 设置为非阻塞模式
    if (!NetworkUtils::set_nonblocking(sock)) {
        std::cerr << "[信令客户端] 设置非阻塞失败" << std::endl;
        CLOSE_SOCKET(sock);
        return INVALID_SOCKET_FD;
    }

    // 3. 构造目标地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(target_port);
    if (inet_pton(AF_INET, target_ip.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "[信令客户端] 无效的IP地址: " << target_ip << std::endl;
        CLOSE_SOCKET(sock);
        return INVALID_SOCKET_FD;
    }

    // 4. 发起连接 (非阻塞)
    // 非阻塞 connect() 通常会返回 -1 且 errno = EINPROGRESS
    // 表示连接正在建立中 (这是正常行为!)
    int conn_result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (conn_result < 0) {
        int err = GET_SOCKET_ERROR();
        if (!NetworkUtils::is_would_block(err)) {
            // 不是 EINPROGRESS, 而是真正的错误
            std::cerr << "[信令客户端] connect() 错误: "
                      << NetworkUtils::get_error_string(err) << std::endl;
            CLOSE_SOCKET(sock);
            return INVALID_SOCKET_FD;
        }
    } else {
        // 连接立即成功 (罕见: 连接到本地主机时可能发生)
        NetworkUtils::set_blocking(sock);
        return sock;
    }

    // 5. 使用 select() 等待连接完成 (带超时)
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);

    struct timeval timeout;
    timeout.tv_sec  = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    int select_result = select((int)(sock + 1), nullptr, &write_fds, nullptr, &timeout);

    if (select_result <= 0) {
        // 超时或错误
        if (select_result == 0) {
            std::cerr << "[信令客户端] 连接超时 (" << timeout_ms << "ms)" << std::endl;
        } else {
            std::cerr << "[信令客户端] select() 错误: "
                      << NetworkUtils::get_last_error_string() << std::endl;
        }
        CLOSE_SOCKET(sock);
        return INVALID_SOCKET_FD;
    }

    // 6. 检查连接是否真的成功
    // select 返回后可写, 但需要检查 SO_ERROR 确认连接是否成功
    int so_error = 0;
    socklen_t so_len = sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR,
                   (char*)&so_error, &so_len) < 0 || so_error != 0) {
        std::cerr << "[信令客户端] 连接失败: "
                  << NetworkUtils::get_error_string(so_error) << std::endl;
        CLOSE_SOCKET(sock);
        return INVALID_SOCKET_FD;
    }

    // 7. 恢复为阻塞模式 (方便后续同步收发)
    NetworkUtils::set_blocking(sock);

    return sock;
}
