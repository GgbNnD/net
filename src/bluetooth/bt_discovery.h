#pragma once

// ============================================================
// 蓝牙设备发现模块 (基于 BlueZ D-Bus)
// 功能: 替代 UDP 组播发现, 通过 BlueZ D-Bus API 扫描附近蓝牙设备
// 原理: 调用 Adapter.StartDiscovery(), 监听 InterfacesAdded 和
//       PropertiesChanged 信号, 解析设备信息并通过回调通知上层
// ============================================================

#include "common/types.h"
#include "common/platform.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <dbus/dbus.h>

// ----------------------------------------------------------
// 收到设备发现时的回调函数类型 (与 DeviceDiscovery 兼容)
// ----------------------------------------------------------
using DeviceFoundCallback = std::function<void(const DeviceInfo& device)>;
using DeviceOfflineCallback = std::function<void(const std::string& device_id)>;

/**
 * @class BtDiscovery
 * @brief 基于 BlueZ D-Bus 的蓝牙设备发现服务
 *
 * 工作流程:
 * 1. 连接 D-Bus 系统总线
 * 2. 获取默认蓝牙适配器, 设置为可被发现
 * 3. 注册 NoInputNoOutput 自动配对 Agent
 * 4. 启动设备扫描 (StartDiscovery)
 * 5. D-Bus 信号监听线程:
 *    - InterfacesAdded: 新设备出现 → 解析 Device1 属性 → 触发 m_device_found_cb
 *    - PropertiesChanged: 已知设备属性更新 → 更新 last_seen
 * 6. 超时清理线程: 周期性检查, 移除超时未活跃的设备
 * 7. stop() 时: 停止扫描, 取消 Agent, 关闭 D-Bus
 *
 * 线程安全: 原子变量控制启停, mutex 保护内部缓存
 */
class BtDiscovery {
public:
    /**
     * @brief 构造函数
     * @param device_id   本机设备UUID
     * @param device_name 本机设备显示名称
     *
     * 注: 蓝牙设备发现不需要端口参数 (与 DeviceDiscovery 不同)
     */
    BtDiscovery(const std::string& device_id,
                const std::string& device_name);

    ~BtDiscovery();

    // ---- 禁止拷贝 ----
    BtDiscovery(const BtDiscovery&) = delete;
    BtDiscovery& operator=(const BtDiscovery&) = delete;

    /**
     * @brief 启动蓝牙设备发现服务
     * @return 是否启动成功
     */
    bool start();

    /**
     * @brief 停止蓝牙设备发现服务
     */
    void stop();

    /**
     * @brief 检查服务是否正在运行
     */
    bool is_running() const { return m_running.load(); }

    /**
     * @brief 设置设备发现回调 (发现新蓝牙设备时触发)
     */
    void set_on_device_found(DeviceFoundCallback callback);

    /**
     * @brief 设置设备离线回调 (设备超时或消失时触发)
     */
    void set_on_device_offline(DeviceOfflineCallback callback);

    /**
     * @brief 获取本机蓝牙地址 (替代 DeviceDiscovery::get_local_ip())
     */
    std::string get_local_addr() const { return m_local_addr; }

private:
    // ---- 核心数据 ----
    std::string  m_device_id;       // 本机设备UUID
    std::string  m_device_name;     // 本机设备显示名称
    std::string  m_local_addr;      // 本机蓝牙地址

    // ---- D-Bus 连接 ----
    DBusConnection* m_dbus_conn;    // D-Bus 系统总线连接
    std::string     m_adapter_path; // BlueZ 适配器路径
    std::string     m_agent_path;   // 配对 Agent 路径

    // ---- 线程管理 ----
    std::thread    m_dispatch_thread;  // D-Bus 消息分发线程
    std::thread    m_cleanup_thread;   // 超时清理线程
    std::atomic<bool> m_running;       // 运行状态标志

    // ---- 回调函数 ----
    DeviceFoundCallback    m_device_found_cb;
    DeviceOfflineCallback  m_device_offline_cb;

    // ---- 内部缓存 ----
    // 已发现设备的 last_seen 跟踪 (device_addr → last_seen)
    mutable std::mutex m_cache_mutex;
    std::map<std::string, DeviceInfo> m_seen_devices;  // 按 BDADDR 索引

    // ---- 内部方法 ----

    /**
     * @brief D-Bus 消息分发线程主函数
     * 循环调用 dispatch_dbus() 处理 incoming signals
     */
    void dispatch_loop();

    /**
     * @brief 超时清理线程主函数
     * 每隔1秒检查已发现设备, 超时则触发离线回调
     */
    void cleanup_loop();

    /**
     * @brief D-Bus 信号过滤器 (静态方法, 通过 user_data 获取 this)
     * 处理 InterfacesAdded 和 PropertiesChanged 信号
     */
    static DBusHandlerResult dbus_filter(DBusConnection* conn,
                                          DBusMessage* msg,
                                          void* user_data);

    /**
     * @brief 处理 InterfacesAdded 信号
     * 从信号参数中提取设备信息并触发回调
     */
    void handle_interfaces_added(DBusMessage* msg);

    /**
     * @brief 处理 PropertiesChanged 信号
     * 更新已知设备的 RSSI 和 last_seen
     */
    void handle_properties_changed(DBusMessage* msg);

    /**
     * @brief 从 D-Bus 消息迭代器解析 DeviceInfo
     * 遍历 Properties 字典, 提取 Address/Name/RSSI/Paired 等字段
     */
    DeviceInfo parse_device_from_properties(DBusMessageIter* props_iter,
                                             const std::string& default_addr = "");

    /**
     * @brief 加载 BlueZ 对象树中已有的设备
     * 调用 GetManagedObjects 获取当前已知设备,
     * 确保启动时不会遗漏已在范围内的设备
     */
    void load_existing_devices();

    /**
     * @brief 从 BlueZ 设备路径提取 BDADDR
     * "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF" → "AA:BB:CC:DD:EE:FF"
     */
    static std::string extract_bdaddr_from_path(const std::string& device_path);
};

// ============================================================
// 蓝牙设备发现模块 - 头文件结束
// ============================================================
