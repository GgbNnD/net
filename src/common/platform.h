#pragma once

// ============================================================
// 跨平台Socket封装
// 功能: 屏蔽 Linux/Windows/macOS 之间Socket API的差异
// ============================================================

#include <cstdint>
#include <string>
#include <stdexcept>
#include <cstring>
#include <vector>

#ifdef _WIN32
    // Windows Socket API
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>

    // Windows 上 socket 文件描述符类型为 SOCKET (uint64_t)
    #define SOCKET_FD SOCKET
    // Windows 无效socket值
    #define INVALID_SOCKET_FD INVALID_SOCKET
    // Windows 关闭socket
    #define CLOSE_SOCKET(s) closesocket(s)
    // Windows 获取错误码
    #define GET_SOCKET_ERROR() WSAGetLastError()
    // Windows socket 发送标志 (send/recv 等同于 write/read)
    #define SOCK_SEND(s, buf, len, flags) send(s, buf, len, flags)
    #define SOCK_RECV(s, buf, len, flags) recv(s, buf, len, flags)

    using socklen_t = int;

#else
    // POSIX Socket API (Linux / macOS)
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/ioctl.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <ifaddrs.h>
    #include <net/if.h>

    // POSIX 上 socket 文件描述符类型为 int
    #define SOCKET_FD int
    // POSIX 无效socket值
    #define INVALID_SOCKET_FD (-1)
    // POSIX 关闭socket
    #define CLOSE_SOCKET(s) close(s)
    // POSIX 获取错误码
    #define GET_SOCKET_ERROR() errno
    // POSIX send/recv
    #define SOCK_SEND(s, buf, len, flags) send(s, buf, len, flags)
    #define SOCK_RECV(s, buf, len, flags) recv(s, buf, len, flags)

#endif

// ============================================================
// 网络工具类: 提供跨平台的网络操作
// ============================================================
namespace NetworkUtils {

/**
 * @brief 初始化网络库 (Windows 需要 WSAStartup, Linux/macOS 无需)
 * @return 是否初始化成功
 */
bool initialize();

/**
 * @brief 清理网络库 (Windows 需要 WSACleanup)
 */
void cleanup();

/**
 * @brief 将整数错误码转换为可读的错误信息字符串
 * @param error_code 错误码
 * @return 错误描述字符串
 */
std::string get_error_string(int error_code);

/**
 * @brief 获取最后一次Socket错误的描述信息
 * @return 错误描述字符串
 */
std::string get_last_error_string();

/**
 * @brief 判断Socket错误是否为"操作会阻塞"的类型
 * @param error_code 错误码
 * @return 如果是 EWOULDBLOCK/EAGAIN/WSAEWOULDBLOCK 返回 true
 */
bool is_would_block(int error_code);

/**
 * @brief 设置Socket为非阻塞模式
 * @param sock socket描述符
 * @return 是否设置成功
 */
bool set_nonblocking(SOCKET_FD sock);

/**
 * @brief 设置Socket地址重用选项
 * @param sock socket描述符
 * @return 是否设置成功
 */
bool set_reuse_addr(SOCKET_FD sock);

/**
 * @brief 获取本机所有IPv4地址
 * @return IPv4地址列表 (字符串形式, 如 "192.168.1.100")
 */
std::vector<std::string> get_local_ips();

} // namespace NetworkUtils
