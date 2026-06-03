#pragma once

// ============================================================
// 在线设备管理器
// 功能: 维护局域网在线设备列表, 处理心跳超时
// ============================================================

#include "common/types.h"
#include "common/platform.h"
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>

/**
 * @class DeviceManager
 * @brief 在线设备列表管理器 (线程安全)
 *
 * 管理规则:
 * 1. 收到设备广播时: 如果设备ID已存在则更新最后活跃时间, 否则添加新设备
 * 2. 每隔1秒检查一次: 移除超过10秒未收到广播的设备
 * 3. 不把自己加入在线列表 (通过 m_self_id 区分)
 * 4. 所有公开方法均为线程安全 (使用互斥锁保护)
 */
class DeviceManager {
public:
    /**
     * @brief 构造函数
     * @param self_device_id 本机设备ID (用于排除自己)
     * @param timeout_seconds 设备超时时间 (秒, 默认10)
     */
    explicit DeviceManager(const std::string& self_device_id,
                            uint32_t timeout_seconds = Defaults::DEVICE_TIMEOUT);

    ~DeviceManager();

    // ---- 禁止拷贝 ----
    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    /**
     * @brief 启动设备管理器 (启动后台清理线程)
     */
    void start();

    /**
     * @brief 停止设备管理器 (停止后台清理线程)
     */
    void stop();

    /**
     * @brief 添加或更新设备 (收到广播时调用)
     * @param device 设备信息 (来自广播消息)
     *
     * 行为:
     * - 如果是本机ID, 忽略
     * - 如果设备ID已存在, 更新 last_seen 时间
     * - 如果设备ID不存在, 添加到列表 (标记为在线)
     */
    void update_device(const DeviceInfo& device);

    /**
     * @brief 移除指定的设备 (收到离线通知或超时时调用)
     * @param device_id 要移除的设备ID
     */
    void remove_device(const std::string& device_id);

    /**
     * @brief 获取所有在线设备的列表
     * @return 在线设备列表的副本
     */
    std::vector<DeviceInfo> get_online_devices() const;

    /**
     * @brief 根据设备ID查找设备
     * @param device_id 设备ID
     * @param out_device 输出: 找到的设备信息
     * @return 找到返回 true
     */
    bool find_device(const std::string& device_id, DeviceInfo& out_device) const;

    /**
     * @brief 获取当前在线设备数量
     * @return 在线设备数 (不包括自己)
     */
    size_t get_device_count() const;

    /**
     * @brief 设置设备上线回调
     * @param callback 回调函数 (新设备上线时触发)
     */
    using DeviceEventCallback = std::function<void(const DeviceInfo& device)>;
    void set_on_device_online(DeviceEventCallback callback);
    void set_on_device_offline(DeviceEventCallback callback);

private:
    // ---- 核心数据 ----
    std::string m_self_id;                          // 本机设备ID
    uint32_t    m_timeout_seconds;                   // 超时时间 (秒)
    std::map<std::string, DeviceInfo> m_devices;     // 设备列表 (key=设备ID)
    mutable std::mutex m_mutex;                      // 互斥锁 (保证线程安全)

    // ---- 后台线程 ----
    std::thread m_cleanup_thread;                    // 超时清理线程
    std::atomic<bool> m_running;                     // 运行状态标志

    // ---- 回调 ----
    DeviceEventCallback m_online_cb;                 // 设备上线回调
    DeviceEventCallback m_offline_cb;                // 设备离线回调

    /**
     * @brief 超时清理线程主函数
     * 每隔1秒检查所有设备, 移除超时未活跃的设备
     */
    void cleanup_loop();
};

// ============================================================
// 在线设备管理器 - 头文件结束
// ============================================================
