// ============================================================
// 设备发现模块 (UDP组播) - 实现
// ============================================================

#include "discovery/device_discovery.h"
#include "common/protocol.h"
#include "common/utils.h"
#include <iostream>
#include <cstring>
#include <chrono>

namespace {

// 接收缓冲区大小 (UDP单包不超过此值)
constexpr size_t RECV_BUFFER_SIZE = 2048;

} // anonymous namespace

// ============================================================
// 构造与析构
// ============================================================

DeviceDiscovery::DeviceDiscovery(const std::string& device_id,
                                   const std::string& device_name,
                                   uint16_t port)
    : m_device_id(device_id)
    , m_device_name(device_name)
    , m_port(port)
    , m_multicast_addr(Defaults::MULTICAST_ADDR)
    , m_socket(INVALID_SOCKET_FD)
    , m_running(false)
{
}

DeviceDiscovery::~DeviceDiscovery() {
    stop();
}

// ============================================================
// 公共接口
// ============================================================

bool DeviceDiscovery::start() {
    if (m_running.load()) {
        return true;  // 已经在运行
    }

    // 选择本机IP
    m_local_ip = select_local_ip();
    if (m_local_ip.empty()) {
        std::cerr << "[发现] 错误: 无法获取本机IP地址" << std::endl;
        return false;
    }

    std::cout << "[发现] 本机IP: " << m_local_ip
              << ", 组播组: " << m_multicast_addr
              << ", 端口: " << m_port << std::endl;

    // 创建UDP组播Socket
    if (!create_socket()) {
        std::cerr << "[发现] 错误: 无法创建UDP组播Socket" << std::endl;
        return false;
    }

    // 设置运行标志
    m_running = true;

    // 启动接收线程 (先启动接收, 避免丢失消息)
    m_recv_thread = std::thread(&DeviceDiscovery::recv_loop, this);

    // 启动发送线程
    m_send_thread = std::thread(&DeviceDiscovery::send_loop, this);

    std::cout << "[发现] 设备发现服务启动成功" << std::endl;
    return true;
}

void DeviceDiscovery::stop() {
    if (!m_running.load()) {
        return;  // 已经停止
    }

    std::cout << "[发现] 正在停止设备发现服务..." << std::endl;

    // 发送离线通知 (广播给所有设备, 立即移除)
    json offline_msg = Protocol::build_device_offline(m_device_id);
    std::string msg_str = offline_msg.dump();

    // 组播地址结构
    struct sockaddr_in multicast_addr;
    memset(&multicast_addr, 0, sizeof(multicast_addr));
    multicast_addr.sin_family = AF_INET;
    multicast_addr.sin_port = htons(m_port);
    inet_pton(AF_INET, m_multicast_addr.c_str(), &multicast_addr.sin_addr);

    // 发送离线通知 (发送3次, 确保对方收到)
    for (int i = 0; i < 3; ++i) {
        sendto(m_socket, msg_str.data(), static_cast<int>(msg_str.size()), 0,
               (struct sockaddr*)&multicast_addr, sizeof(multicast_addr));
    }

    // 停止线程
    m_running = false;

    // 关闭Socket (recvfrom会立即返回错误, 线程退出)
    close_socket();

    // 等待线程结束
    if (m_send_thread.joinable()) {
        m_send_thread.join();
    }
    if (m_recv_thread.joinable()) {
        m_recv_thread.join();
    }

    std::cout << "[发现] 设备发现服务已停止" << std::endl;
}

void DeviceDiscovery::set_on_device_found(DeviceFoundCallback callback) {
    m_device_found_cb = std::move(callback);
}

void DeviceDiscovery::set_on_device_offline(DeviceOfflineCallback callback) {
    m_device_offline_cb = std::move(callback);
}

// ============================================================
// 内部方法
// ============================================================

bool DeviceDiscovery::create_socket() {
    // 1. 创建UDP Socket
    m_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socket == INVALID_SOCKET_FD) {
        std::cerr << "[发现] socket() 失败: " << NetworkUtils::get_last_error_string() << std::endl;
        return false;
    }

    // 2. 设置地址重用 (允许多个程序绑定同一端口)
    if (!NetworkUtils::set_reuse_addr(m_socket)) {
        std::cerr << "[发现] setsockopt(SO_REUSEADDR) 失败" << std::endl;
        close_socket();
        return false;
    }

    // 3. 绑定到固定端口 (使用 INADDR_ANY 监听所有网络接口)
    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(m_port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(m_socket, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        std::cerr << "[发现] bind() 失败: " << NetworkUtils::get_last_error_string() << std::endl;
        close_socket();
        return false;
    }

    // 4. 加入组播组
    // 原理: 通知操作系统, 本机对该组播地址感兴趣,
    //       当其他设备向该组播地址发送数据时, OS会转发一份给我们
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(m_multicast_addr.c_str());  // 组播地址
    mreq.imr_interface.s_addr = inet_addr(m_local_ip.c_str());         // 本机IP

    if (setsockopt(m_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   (const char*)&mreq, sizeof(mreq)) < 0) {
        std::cerr << "[发现] setsockopt(IP_ADD_MEMBERSHIP) 失败: "
                  << NetworkUtils::get_last_error_string() << std::endl;
        close_socket();
        return false;
    }

    // 5. 设置组播TTL (生存时间) = 1, 限制在本地网络内, 不路由到外网
    int ttl = 1;
    if (setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_TTL,
                   (const char*)&ttl, sizeof(ttl)) < 0) {
        std::cerr << "[发现] 警告: 设置组播TTL失败" << std::endl;
        // 非致命错误, 继续执行
    }

    // 6. 设置发送组播的接口
    struct in_addr iface;
    iface.s_addr = inet_addr(m_local_ip.c_str());
    if (setsockopt(m_socket, IPPROTO_IP, IP_MULTICAST_IF,
                   (const char*)&iface, sizeof(iface)) < 0) {
        std::cerr << "[发现] 警告: 设置组播输出接口失败" << std::endl;
        // 非致命错误, 继续执行
    }

    return true;
}

void DeviceDiscovery::close_socket() {
    if (m_socket != INVALID_SOCKET_FD) {
        CLOSE_SOCKET(m_socket);
        m_socket = INVALID_SOCKET_FD;
    }
}

// ----------------------------------------------------------
// 广播发送循环
// ----------------------------------------------------------
void DeviceDiscovery::send_loop() {
    std::cout << "[发现] 广播发送线程启动 (间隔 "
              << Defaults::BROADCAST_INTERVAL << " 秒)" << std::endl;

    // 组播目标地址 (构造函数外, 避免每次发送时重新构造)
    struct sockaddr_in multicast_addr;
    memset(&multicast_addr, 0, sizeof(multicast_addr));
    multicast_addr.sin_family = AF_INET;
    multicast_addr.sin_port = htons(m_port);
    inet_pton(AF_INET, m_multicast_addr.c_str(), &multicast_addr.sin_addr);

    while (m_running.load()) {
        // 构建设备广播消息 (JSON格式)
        uint64_t now = Utils::get_timestamp_ms();
        json msg = Protocol::build_device_broadcast(
            m_device_id, m_device_name, m_local_ip, m_port, now
        );
        std::string msg_str = msg.dump();

        // 发送组播消息
        auto sent = sendto(m_socket, msg_str.data(),
                           static_cast<int>(msg_str.size()), 0,
                           (struct sockaddr*)&multicast_addr,
                           sizeof(multicast_addr));

        if (sent < 0) {
            int err = GET_SOCKET_ERROR();
            if (err != 0) {
                std::cerr << "[发现] 发送广播失败: "
                          << NetworkUtils::get_error_string(err) << std::endl;
            }
        }

        // 等待下一次发送 (使用短暂休眠, 响应停止指令)
        for (uint32_t i = 0; i < Defaults::BROADCAST_INTERVAL * 10 && m_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::cout << "[发现] 广播发送线程退出" << std::endl;
}

// ----------------------------------------------------------
// 消息接收循环
// ----------------------------------------------------------
void DeviceDiscovery::recv_loop() {
    std::cout << "[发现] 消息接收线程启动" << std::endl;

    char buffer[RECV_BUFFER_SIZE];

    while (m_running.load()) {
        struct sockaddr_in sender_addr;
        socklen_t sender_len = sizeof(sender_addr);

        // 阻塞接收 (send_loop关闭socket后, recvfrom会立即返回错误)
        auto recv_len = recvfrom(m_socket, buffer, RECV_BUFFER_SIZE - 1, 0,
                                 (struct sockaddr*)&sender_addr, &sender_len);

        if (recv_len < 0) {
            // Socket被关闭或出现错误, 退出循环
            break;
        }

        // 确保字符串终止
        buffer[recv_len] = '\0';

        // 解析JSON消息
        try {
            json msg = json::parse(buffer);

            // 检查消息类型
            std::string type = msg.value("type", "");
            if (type == MsgType::DEVICE_BROADCAST) {
                // 收到设备广播: 解析设备信息
                DeviceInfo device;
                device.id   = msg.value("device_id", "");
                device.name = msg.value("device_name", "");
                device.ip   = msg.value("ip", "");
                device.port = msg.value("port", 0);
                device.last_seen = std::chrono::steady_clock::now();
                device.first_seen = std::chrono::steady_clock::now();

                // 提取发送方IP (从sockaddr中获取, 更可靠)
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sender_addr.sin_addr, ip_str, sizeof(ip_str));
                device.ip = ip_str;

                // 忽略来自自己的广播
                if (device.id == m_device_id) {
                    continue;
                }

                // 触发回调
                if (m_device_found_cb) {
                    m_device_found_cb(device);
                }

                std::cout << "[发现] 发现设备: " << device.name
                          << " (" << device.ip << ":" << device.port << ")"
                          << std::endl;

            } else if (type == MsgType::DEVICE_OFFLINE) {
                // 收到设备离线通知
                std::string device_id = msg.value("device_id", "");

                // 忽略自己
                if (device_id == m_device_id) {
                    continue;
                }

                // 触发回调
                if (m_device_offline_cb) {
                    m_device_offline_cb(device_id);
                }

                std::cout << "[发现] 设备下线: " << device_id << std::endl;
            }

        } catch (const json::exception& e) {
            // JSON解析失败 (可能是其他程序发送的无关数据包)
            // 忽略并继续等待下一条消息
        }
    }

    std::cout << "[发现] 消息接收线程退出" << std::endl;
}

// ----------------------------------------------------------
// 选择本机通信IP
// ----------------------------------------------------------
std::string DeviceDiscovery::select_local_ip() {
    auto ips = NetworkUtils::get_local_ips();

    if (ips.empty()) {
        return "";
    }

    // 返回第一个非回环IPv4地址
    return ips[0];
}
