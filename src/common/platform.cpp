// ============================================================
// 跨平台Socket封装 - 实现
// ============================================================

#include "common/platform.h"
#include <sstream>
#include <vector>
#include <utility>

#ifdef _WIN32
    #pragma comment(lib, "ws2_32.lib")
#endif

namespace NetworkUtils {

// ----------------------------------------------------------
// 初始化网络库
// ----------------------------------------------------------
bool initialize() {
#ifdef _WIN32
    // Windows: 必须调用 WSAStartup 初始化 Winsock DLL
    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        return false;
    }
#endif
    return true;
}

// ----------------------------------------------------------
// 清理网络库
// ----------------------------------------------------------
void cleanup() {
#ifdef _WIN32
    // Windows: 清理 Winsock
    WSACleanup();
#endif
}

// ----------------------------------------------------------
// 获取错误描述字符串
// ----------------------------------------------------------
std::string get_error_string(int error_code) {
#ifdef _WIN32
    LPVOID msg_buf = nullptr;
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&msg_buf, 0, nullptr
    );
    if (msg_buf) {
        std::string msg((char*)msg_buf);
        LocalFree(msg_buf);
        // 去除尾部换行符
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
            msg.pop_back();
        }
        return msg;
    }
    return "Unknown error: " + std::to_string(error_code);
#else
    return std::string(strerror(error_code));
#endif
}

/**
 * @brief 获取最后一次Socket错误的描述信息
 *
 * @details
 * 在 Windows 上获取 WSAGetLastError() 的结果
 * 在 Linux/macOS 上获取 errno 的当前值
 *
 * @return std::string 错误信息字符串，包含错误码和描述
 */
std::string get_last_error_string() {
    int err = GET_SOCKET_ERROR();
    return "[错误码 " + std::to_string(err) + "] " + get_error_string(err);
}

// ----------------------------------------------------------
// 判断是否为"操作会阻塞"错误
// ----------------------------------------------------------
bool is_would_block(int error_code) {
#ifdef _WIN32
    return error_code == WSAEWOULDBLOCK;
#else
    // EINPROGRESS: 非阻塞 connect() 正在建立连接 (正常状态!)
    // EWOULDBLOCK / EAGAIN: 非阻塞 read/write 时缓冲区无数据/已满
    return error_code == EWOULDBLOCK || error_code == EAGAIN || error_code == EINPROGRESS;
#endif
}

// ----------------------------------------------------------
// 设置Socket为非阻塞模式
// ----------------------------------------------------------
bool set_nonblocking(SOCKET_FD sock) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// ----------------------------------------------------------
// 设置Socket为阻塞模式
// ----------------------------------------------------------
bool set_blocking(SOCKET_FD sock) {
#ifdef _WIN32
    u_long mode = 0;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(sock, F_SETFL, flags & ~O_NONBLOCK) == 0;
#endif
}

// ----------------------------------------------------------
// 设置Socket地址重用
// ----------------------------------------------------------
bool set_reuse_addr(SOCKET_FD sock) {
    int optval = 1;
#ifdef _WIN32
    return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                      (const char*)&optval, sizeof(optval)) == 0;
#else
    return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                      &optval, sizeof(optval)) == 0;
#endif
}

// ----------------------------------------------------------
// 获取本机所有IPv4地址
// ----------------------------------------------------------
std::vector<std::string> get_local_ips() {
    std::vector<std::string> ips;

#ifdef _WIN32
    // Windows: 使用 getaddrinfo 获取本机地址
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        return ips;
    }

    struct addrinfo hints, *result = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        // 仅IPv4
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0) {
        return ips;
    }

    for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        struct sockaddr_in* addr = (struct sockaddr_in*)ptr->ai_addr;
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
        ips.push_back(ip_str);
    }
    freeaddrinfo(result);

#else
    // Linux/macOS: 遍历网络接口获取IPv4地址
    struct ifaddrs* ifaddr;
    if (getifaddrs(&ifaddr) == -1) {
        return ips;
    }

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        // 只处理IPv4地址
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        // 跳过回环地址127.0.0.1
        struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
        std::string ip(ip_str);
        if (ip != "127.0.0.1") {
            ips.push_back(ip);
        }
    }
    freeifaddrs(ifaddr);
#endif

    return ips;
}

// ----------------------------------------------------------
// 获取本机IPv4地址和子网掩码
// ----------------------------------------------------------
std::vector<std::pair<std::string, std::string>> get_local_ips_with_mask() {
    std::vector<std::pair<std::string, std::string>> result;

#ifdef _WIN32
    // Windows: 简单返回没有子网掩码的IP
    for (const auto& ip : get_local_ips()) {
        result.emplace_back(ip, "255.255.255.0");  // 默认 /24
    }
#else
    struct ifaddrs* ifaddr;
    if (getifaddrs(&ifaddr) == -1) {
        return result;
    }

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr || ifa->ifa_netmask == nullptr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;

        struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
        struct sockaddr_in* mask = (struct sockaddr_in*)ifa->ifa_netmask;

        char ip_str[INET_ADDRSTRLEN];
        char mask_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
        inet_ntop(AF_INET, &mask->sin_addr, mask_str, sizeof(mask_str));

        std::string ip(ip_str);
        if (ip != "127.0.0.1") {
            result.emplace_back(ip, std::string(mask_str));
        }
    }
    freeifaddrs(ifaddr);
#endif

    return result;
}

} // namespace NetworkUtils
