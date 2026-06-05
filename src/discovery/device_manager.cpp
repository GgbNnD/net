// ============================================================
// 在线设备管理器 - 实现
// ============================================================

#include "discovery/device_manager.h"
#include "common/utils.h"
#include <iostream>
#include <algorithm>

// ============================================================
// 构造与析构
// ============================================================

DeviceManager::DeviceManager(const std::string& self_device_id,
                               uint32_t timeout_seconds)
    : m_self_id(self_device_id)
    , m_timeout_seconds(timeout_seconds)
    , m_running(false)
{
}

DeviceManager::~DeviceManager() {
    stop();
}

// ============================================================
// 公共接口
// ============================================================

void DeviceManager::start() {
    if (m_running.load()) {
        return;
    }

    m_running = true;
    m_cleanup_thread = std::thread(&DeviceManager::cleanup_loop, this);

    std::cout << "[管理器] 设备管理器启动 (超时: "
              << m_timeout_seconds << " 秒)" << std::endl;
}

void DeviceManager::stop() {
    if (!m_running.load()) {
        return;
    }

    m_running = false;

    if (m_cleanup_thread.joinable()) {
        m_cleanup_thread.join();
    }

    std::cout << "[管理器] 设备管理器已停止" << std::endl;
}

void DeviceManager::update_device(const DeviceInfo& device) {
    // 忽略来自自己的广播
    if (device.id == m_self_id) {
        return;
    }

    std::unique_lock<std::mutex> lock(m_mutex);

    bool is_new_device = false;
    DeviceInfo cb_device;  // 用于在锁外触发回调

    auto it = m_devices.find(device.id);
    if (it == m_devices.end()) {
        // 新设备上线: 添加到列表
        DeviceInfo new_device = device;
        new_device.last_seen = std::chrono::steady_clock::now();
        new_device.first_seen = std::chrono::steady_clock::now();
        m_devices[device.id] = new_device;

        std::cout << "[管理器] 新设备上线: " << device.name
                  << " (" << device.id.substr(0, 8) << "...)"
                   << " [" << device.addr << ":" << device.port << "]"
                   << std::endl;

        is_new_device = true;
        cb_device = new_device;
    } else {
        // 已知设备: 更新最后活跃时间
        it->second.last_seen = std::chrono::steady_clock::now();
        // 更新可能变化的字段
        it->second.name = device.name;
        it->second.addr = device.addr;
        it->second.port = device.port;
        it->second.manual = device.manual || it->second.manual;
        // 仅当 incoming device 携带有效的探活时间时才更新
        // (避免蓝牙扫描结果覆盖 RFCOMM 探活线程设置的 connected 状态)
        if (device.last_probed.time_since_epoch().count() > 0) {
            it->second.last_probed = device.last_probed;
            it->second.connected = device.connected;
        }
    }

    // 解锁后再触发回调, 避免死锁
    lock.unlock();

    if (is_new_device && m_online_cb) {
        m_online_cb(cb_device);
    }
}

void DeviceManager::remove_device(const std::string& device_id) {
    std::unique_lock<std::mutex> lock(m_mutex);

    auto it = m_devices.find(device_id);
    if (it != m_devices.end()) {
        DeviceInfo offline_device = it->second;
        std::cout << "[管理器] 设备离线: " << offline_device.name
                  << " (" << device_id.substr(0, 8) << "...)" << std::endl;
        m_devices.erase(it);

        // 解锁后触发回调, 避免死锁
        lock.unlock();

        if (m_offline_cb) {
            m_offline_cb(offline_device);
        }
    }
}

std::vector<DeviceInfo> DeviceManager::get_online_devices() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<DeviceInfo> result;
    result.reserve(m_devices.size());
    for (const auto& pair : m_devices) {
        result.push_back(pair.second);
    }
    return result;
}

bool DeviceManager::find_device(const std::string& device_id,
                                 DeviceInfo& out_device) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_devices.find(device_id);
    if (it != m_devices.end()) {
        out_device = it->second;
        return true;
    }
    return false;
}

size_t DeviceManager::get_device_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_devices.size();
}

std::string DeviceManager::find_device_id_by_addr(const std::string& addr) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& pair : m_devices) {
        if (pair.second.addr == addr) return pair.first;
    }
    return "";
}

void DeviceManager::set_on_device_online(DeviceEventCallback callback) {
    m_online_cb = std::move(callback);
}

void DeviceManager::set_on_device_offline(DeviceEventCallback callback) {
    m_offline_cb = std::move(callback);
}

// ============================================================
// 内部方法: 超时清理循环
// ============================================================

void DeviceManager::cleanup_loop() {
    std::cout << "[管理器] 超时清理线程启动" << std::endl;

    while (m_running.load()) {
        // 每隔1秒检查一次
        std::this_thread::sleep_for(std::chrono::seconds(1));

        std::vector<std::string> expired_ids;
        auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // 遍历所有设备, 找到超时的 (手动添加的设备不会超时)
            for (auto it = m_devices.begin(); it != m_devices.end(); ) {
                // 手动添加的设备跳过超时清理
                if (it->second.manual) {
                    ++it;
                    continue;
                }

                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - it->second.last_seen
                ).count();

                if (elapsed > m_timeout_seconds) {
                    std::cout << "[管理器] 设备超时离线: " << it->second.name
                              << " (未活跃 " << elapsed << " 秒)" << std::endl;
                    expired_ids.push_back(it->first);
                    it = m_devices.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // 在锁外触发离线回调 (避免死锁)
        for (const auto& id : expired_ids) {
            if (m_offline_cb) {
                DeviceInfo offline;
                offline.id = id;
                m_offline_cb(offline);
            }
        }
    }

    std::cout << "[管理器] 超时清理线程退出" << std::endl;
}
