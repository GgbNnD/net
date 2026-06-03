#pragma once

// ============================================================
// 设备发现模块 (UDP组播)
// 功能: 通过UDP组播实现局域网内设备的自动发现
// 原理: 每台设备周期性向组播地址发送"存在信息"，
//       同时监听组播地址接收其他设备的信息包
// ============================================================

#include "common/types.h"
#include "common/platform.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>

// ----------------------------------------------------------
// 收到设备广播时的回调函数类型
// 参数: DeviceInfo - 新发现或更新的设备信息
// ----------------------------------------------------------
using DeviceFoundCallback = std::function<void(const DeviceInfo& device)>;

// ----------------------------------------------------------
// 收到设备离线通知时的回调函数类型
// 参数: device_id - 离线的设备ID
// ----------------------------------------------------------
using DeviceOfflineCallback = std::function<void(const std::string& device_id)>;

/**
 * @class DeviceDiscovery
 * @brief UDP组播设备发现服务
 *
 * 工作流程:
 * 1. 创建UDP Socket, 绑定到固定端口 (8888)
 * 2. 加入组播组 (239.255.255.250), 设置地址重用
 * 3. 启动发送线程: 每隔3秒向组播地址发送 DEVICE_BROADCAST 消息
 * 4. 启动接收线程: 循环接收组播消息, 解析后通过回调通知上层
 * 5. 停止时发送 DEVICE_OFFLINE 消息并关闭socket
 *
 * 线程安全: 使用原子变量控制线程启停, socket操作在主线程
 */
class DeviceDiscovery {
public:
    /**
     * @brief 构造函数
     * @param device_id   本机设备唯一ID
     * @param device_name 本机设备显示名称
     * @param port        绑定的UDP端口 (默认 8888)
     */
    DeviceDiscovery(const std::string& device_id,
                     const std::string& device_name,
                     uint16_t port = Defaults::DISCOVERY_PORT);

    ~DeviceDiscovery();

    // ---- 禁止拷贝 ----
    DeviceDiscovery(const DeviceDiscovery&) = delete;
    DeviceDiscovery& operator=(const DeviceDiscovery&) = delete;

    /**
     * @brief 启动设备发现服务
     * 创建UDP socket, 加入组播组, 启动收发线程
     * @return 是否启动成功
     */
    bool start();

    /**
     * @brief 停止设备发现服务
     * 发送离线通知, 停止线程, 关闭socket
     */
    void stop();

    /**
     * @brief 检查服务是否正在运行
     * @return 运行中返回 true
     */
    bool is_running() const { return m_running.load(); }

    /**
     * @brief 设置设备发现回调 (收到其他设备广播时触发)
     * @param callback 回调函数
     */
    void set_on_device_found(DeviceFoundCallback callback);

    /**
     * @brief 设置设备离线回调 (收到其他设备离线通知时触发)
     * @param callback 回调函数
     */
    void set_on_device_offline(DeviceOfflineCallback callback);

    /**
     * @brief 获取本机绑定的IP地址
     * @return IP地址字符串
     */
    std::string get_local_ip() const { return m_local_ip; }

private:
    // ---- 核心数据 ----
    std::string    m_device_id;       // 本机设备ID
    std::string    m_device_name;     // 本机设备名称
    std::string    m_local_ip;        // 本机IP地址
    uint16_t       m_port;            // 绑定端口
    std::string    m_multicast_addr;  // 组播地址

    // ---- Socket 相关 ----
    SOCKET_FD      m_socket;          // UDP socket描述符

    // ---- 线程管理 ----
    std::thread    m_send_thread;     // 广播发送线程
    std::thread    m_recv_thread;     // 消息接收线程
    std::atomic<bool> m_running;      // 运行状态标志

    // ---- 回调函数 ----
    DeviceFoundCallback    m_device_found_cb;    // 发现设备回调
    DeviceOfflineCallback  m_device_offline_cb;  // 设备离线回调

    // ---- 内部方法 ----

    /**
     * @brief 创建并配置UDP组播Socket
     * 步骤: socket() -> setsockopt(REUSEADDR) -> bind() -> setsockopt(ADD_MEMBERSHIP)
     * @return 是否创建成功
     */
    bool create_socket();

    /**
     * @brief 关闭UDP Socket
     */
    void close_socket();

    /**
     * @brief 广播发送线程主函数
     * 每隔3秒发送一次设备信息广播
     */
    void send_loop();

    /**
     * @brief 消息接收线程主函数
     * 循环接收组播消息, 解析并触发回调
     */
    void recv_loop();

    /**
     * @brief 选择本机用于通信的IP地址
     * 优先选择非回环IPv4地址
     */
    std::string select_local_ip();
};

// ============================================================
// 设备发现模块 (UDP组播) - 头文件结束
// ============================================================
