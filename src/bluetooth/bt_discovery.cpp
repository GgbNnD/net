// ============================================================
// 蓝牙设备发现模块 - 实现
// ============================================================

#include "bluetooth/bt_discovery.h"
#include "bluetooth/bt_utils.h"
#include "common/utils.h"
#include <iostream>
#include <cstring>
#include <chrono>

// 设备超时时间 (秒): 超过此时间未收到信号则视为离线
static constexpr uint32_t DEVICE_TIMEOUT_SEC = 15;

// D-Bus 消息分发轮询间隔 (毫秒)
static constexpr int DISPATCH_INTERVAL_MS = 100;

// ============================================================
// 构造与析构
// ============================================================

BtDiscovery::BtDiscovery(const std::string& device_id,
                          const std::string& device_name)
    : m_device_id(device_id)
    , m_device_name(device_name)
    , m_dbus_conn(nullptr)
    , m_running(false)
{
}

BtDiscovery::~BtDiscovery() {
    stop();
}

// ============================================================
// 公共接口
// ============================================================

bool BtDiscovery::start() {
    if (m_running.load()) return true;

    // 1. 获取本机蓝牙地址
    m_local_addr = BtUtils::get_local_bdaddr();
    if (m_local_addr.empty()) {
        std::cerr << "[蓝牙发现] 未检测到蓝牙适配器" << std::endl;
        return false;
    }
    std::cout << "[蓝牙发现] 本机蓝牙地址: " << m_local_addr << std::endl;

    // 2. 连接 D-Bus 系统总线
    if (!BtUtils::init_dbus(m_dbus_conn)) {
        return false;
    }

    // 3. 获取默认适配器路径
    m_adapter_path = BtUtils::get_default_adapter_path(m_dbus_conn);
    if (m_adapter_path.empty()) {
        std::cerr << "[蓝牙发现] 未找到 BlueZ 适配器" << std::endl;
        BtUtils::cleanup_dbus(m_dbus_conn);
        m_dbus_conn = nullptr;
        return false;
    }

    // 4. 注册自动配对 Agent (接受所有配对请求)
    m_agent_path = BtUtils::register_auto_pair_agent(m_dbus_conn, m_adapter_path);

    // 5. 添加 Agent 消息处理器到 D-Bus 过滤器
    BtUtils::add_dbus_filter(m_dbus_conn, BtUtils::handle_agent_message, nullptr);

    // 6. 设置适配器属性: 可被发现 + 可被配对
    BtUtils::set_adapter_property(m_dbus_conn, m_adapter_path,
                                   "Discoverable", true);
    BtUtils::set_adapter_property(m_dbus_conn, m_adapter_path,
                                   "Pairable", true);

    // 设置适配器别名 (设备显示名称)
    // 需要通过 D-Bus Properties.Set 设置 Alias
    {
        DBusMessage* msg = dbus_message_new_method_call(
            "org.bluez", m_adapter_path.c_str(),
            "org.freedesktop.DBus.Properties", "Set");
        if (msg) {
            DBusMessageIter iter;
            dbus_message_iter_init_append(msg, &iter);
            const char* iface = "org.bluez.Adapter1";
            const char* prop  = "Alias";
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface);
            dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &prop);
            DBusMessageIter variant;
            dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
                                              DBUS_TYPE_STRING_AS_STRING, &variant);
            const char* name_cstr = m_device_name.c_str();
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &name_cstr);
            dbus_message_iter_close_container(&iter, &variant);

            DBusError error;
            dbus_error_init(&error);
            DBusMessage* reply = dbus_connection_send_with_reply_and_block(
                m_dbus_conn, msg, 3000, &error);
            if (dbus_error_is_set(&error)) {
                std::cerr << "[蓝牙发现] 设置 Alias 失败: " << error.message << std::endl;
                dbus_error_free(&error);
            }
            if (reply) dbus_message_unref(reply);
            dbus_message_unref(msg);
        }
    }

    // 7. 添加本模块的 D-Bus 过滤器 (监听设备信号)
    BtUtils::add_dbus_filter(m_dbus_conn, dbus_filter, this);

    // 8. 启动设备扫描
    if (!BtUtils::start_discovery(m_dbus_conn, m_adapter_path)) {
        std::cerr << "[蓝牙发现] 启动扫描失败" << std::endl;
        BtUtils::remove_dbus_filter(m_dbus_conn, dbus_filter, this);
        BtUtils::remove_dbus_filter(m_dbus_conn, BtUtils::handle_agent_message, nullptr);
        BtUtils::cleanup_dbus(m_dbus_conn);
        m_dbus_conn = nullptr;
        return false;
    }

    // 9. 启动后台线程
    m_running = true;
    m_dispatch_thread = std::thread(&BtDiscovery::dispatch_loop, this);
    m_cleanup_thread  = std::thread(&BtDiscovery::cleanup_loop, this);

    std::cout << "[蓝牙发现] 设备发现服务已启动" << std::endl;
    return true;
}

void BtDiscovery::stop() {
    if (!m_running.load()) return;

    std::cout << "[蓝牙发现] 正在停止设备发现服务..." << std::endl;
    m_running = false;

    // 1. 停止扫描
    if (m_dbus_conn && !m_adapter_path.empty()) {
        BtUtils::stop_discovery(m_dbus_conn, m_adapter_path);
    }

    // 2. 等待线程退出
    if (m_cleanup_thread.joinable()) {
        m_cleanup_thread.join();
    }
    if (m_dispatch_thread.joinable()) {
        m_dispatch_thread.join();
    }

    // 3. 取消 Agent 和过滤器
    if (m_dbus_conn) {
        BtUtils::remove_dbus_filter(m_dbus_conn, dbus_filter, this);
        BtUtils::remove_dbus_filter(m_dbus_conn, BtUtils::handle_agent_message, nullptr);

        if (!m_agent_path.empty()) {
            BtUtils::unregister_auto_pair_agent(m_dbus_conn, m_adapter_path, m_agent_path);
        }

        // 关闭适配器可发现性
        BtUtils::set_adapter_property(m_dbus_conn, m_adapter_path,
                                       "Discoverable", false);

        BtUtils::cleanup_dbus(m_dbus_conn);
        m_dbus_conn = nullptr;
    }

    std::cout << "[蓝牙发现] 设备发现服务已停止" << std::endl;
}

void BtDiscovery::set_on_device_found(DeviceFoundCallback callback) {
    m_device_found_cb = std::move(callback);
}

void BtDiscovery::set_on_device_offline(DeviceOfflineCallback callback) {
    m_device_offline_cb = std::move(callback);
}

// ============================================================
// 后台线程
// ============================================================

void BtDiscovery::dispatch_loop() {
    std::cout << "[蓝牙发现] D-Bus 消息分发线程启动" << std::endl;

    while (m_running.load()) {
        if (m_dbus_conn) {
            BtUtils::dispatch_dbus(m_dbus_conn, DISPATCH_INTERVAL_MS);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(DISPATCH_INTERVAL_MS));
        }
    }

    std::cout << "[蓝牙发现] D-Bus 消息分发线程退出" << std::endl;
}

void BtDiscovery::cleanup_loop() {
    std::cout << "[蓝牙发现] 超时清理线程启动" << std::endl;

    while (m_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        std::vector<std::string> expired_addrs;

        {
            std::lock_guard<std::mutex> lock(m_cache_mutex);
            auto now = std::chrono::steady_clock::now();

            for (auto it = m_seen_devices.begin(); it != m_seen_devices.end(); ) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - it->second.last_seen).count();

                if (elapsed > DEVICE_TIMEOUT_SEC) {
                    std::cout << "[蓝牙发现] 设备超时: " << it->second.name
                              << " (" << it->first << "), "
                              << elapsed << " 秒未活跃" << std::endl;
                    expired_addrs.push_back(it->first);
                    it = m_seen_devices.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // 在锁外触发回调
        for (const auto& addr : expired_addrs) {
            if (m_device_offline_cb) {
                // 通过 BDADDR 查找 device_id
                // 使用特殊格式: 地址作为 device_id
                m_device_offline_cb(addr);
            }
        }
    }

    std::cout << "[蓝牙发现] 超时清理线程退出" << std::endl;
}

// ============================================================
// D-Bus 信号过滤器
// ============================================================

DBusHandlerResult BtDiscovery::dbus_filter(DBusConnection* /*conn*/,
                                            DBusMessage* msg,
                                            void* user_data) {
    BtDiscovery* self = static_cast<BtDiscovery*>(user_data);
    if (!self) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    // 只处理信号
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char* iface = dbus_message_get_interface(msg);
    const char* member = dbus_message_get_member(msg);

    if (!iface || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    // InterfacesAdded 信号: 新设备出现
    if (strcmp(iface, "org.freedesktop.DBus.ObjectManager") == 0 &&
        strcmp(member, "InterfacesAdded") == 0) {
        self->handle_interfaces_added(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    // PropertiesChanged 信号: 已知设备属性更新
    if (strcmp(iface, "org.freedesktop.DBus.Properties") == 0 &&
        strcmp(member, "PropertiesChanged") == 0) {
        self->handle_properties_changed(msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

// ============================================================
// 信号处理
// ============================================================

void BtDiscovery::handle_interfaces_added(DBusMessage* msg) {
    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter)) return;

    // ARG0: OBJECT_PATH (设备路径, 如 "/org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX")
    // 跳过对象路径字段
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
        // 路径已获取, 继续处理 ARG1
    } else {
        return;
    }

    // ARG1: 接口字典 (array of dict_entries)
    if (!dbus_message_iter_next(&iter)) return;

    // 遍历字典: 找到 org.bluez.Device1 的接口条目
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) return;

    DBusMessageIter dict_iter;
    dbus_message_iter_recurse(&iter, &dict_iter);

    while (dbus_message_iter_get_arg_type(&dict_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, props_array;
        dbus_message_iter_recurse(&dict_iter, &entry);

        const char* iface_name = nullptr;
        dbus_message_iter_get_basic(&entry, &iface_name);

        // 只处理 Device1 接口
        if (!iface_name || strcmp(iface_name, "org.bluez.Device1") != 0) {
            dbus_message_iter_next(&dict_iter);
            continue;
        }

        // 获取属性字典
        if (!dbus_message_iter_next(&entry)) break;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&entry, &props_array);
            DeviceInfo device = parse_device_from_properties(&props_array);

            // 跳过本机
            if (device.addr == m_local_addr) {
                break;
            }

            if (!device.addr.empty()) {
                // 更新或添加设备
                bool is_new = false;
                {
                    std::lock_guard<std::mutex> lock(m_cache_mutex);
                    auto it = m_seen_devices.find(device.addr);
                    if (it == m_seen_devices.end()) {
                        device.id = Utils::generate_uuid();
                        device.last_seen = std::chrono::steady_clock::now();
                        device.first_seen = std::chrono::steady_clock::now();
                        m_seen_devices[device.addr] = device;
                        is_new = true;
                    } else {
                        it->second.last_seen = std::chrono::steady_clock::now();
                        it->second.name = device.name;
                    }
                }

                if (is_new && m_device_found_cb) {
                    // 使用缓存的 device (含完整信息)
                    std::lock_guard<std::mutex> lock(m_cache_mutex);
                    m_device_found_cb(m_seen_devices[device.addr]);
                }
            }
        }
        break;
    }
}

void BtDiscovery::handle_properties_changed(DBusMessage* msg) {
    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter)) return;

    // ARG0: STRING (接口名称)
    const char* iface_name = nullptr;
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
        dbus_message_iter_get_basic(&iter, &iface_name);
    } else {
        return;
    }

    // 只处理 Device1 的 PropertiesChanged
    if (!iface_name || strcmp(iface_name, "org.bluez.Device1") != 0) {
        return;
    }

    // 获取设备路径 (从 D-Bus 消息的 path 字段获取)
    // device path 格式: "/org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX"
    // 从中提取 BDADDR: 最后 17 个字符
    const char* device_path = dbus_message_get_path(msg);
    if (!device_path) return;

    // 从路径提取地址: .../dev_XX_XX_XX_XX_XX_XX
    const char* underscore = strrchr(device_path, '_');
    std::string addr;
    if (underscore && strlen(underscore) >= 18) {
        // 将下划线转换为冒号: dev_AA_BB_CC_DD_EE_FF → AA:BB:CC:DD:EE:FF
        addr.reserve(17);
        for (int i = 1; i <= 17; ++i) {
            char c = underscore[i];
            if (c == '_') {
                addr += ':';
            } else {
                addr += c;
            }
        }
    }

    if (addr.empty() || addr == m_local_addr) return;

    // 更新 last_seen
    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        auto it = m_seen_devices.find(addr);
        if (it != m_seen_devices.end()) {
            it->second.last_seen = std::chrono::steady_clock::now();

            // 检查 RSSI 更新
            if (dbus_message_iter_next(&iter)) {
                DBusMessageIter changed_iter;
                if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
                    dbus_message_iter_recurse(&iter, &changed_iter);
                    // 遍历变更属性 (可从中提取 Name/RSSI 更新)
                    while (dbus_message_iter_get_arg_type(&changed_iter) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter prop_entry;
                        dbus_message_iter_recurse(&changed_iter, &prop_entry);

                        const char* prop_name = nullptr;
                        dbus_message_iter_get_basic(&prop_entry, &prop_name);

                        if (prop_name && dbus_message_iter_next(&prop_entry)) {
                            // 尝试获取 RSSI
                            if (strcmp(prop_name, "RSSI") == 0 ||
                                strcmp(prop_name, "Name") == 0) {
                                // 属性值在 variant 中, 跳过处理
                            }
                        }
                        dbus_message_iter_next(&changed_iter);
                    }
                }
            }
        }
    }
}

// ============================================================
// DeviceInfo 解析
// ============================================================

DeviceInfo BtDiscovery::parse_device_from_properties(
    DBusMessageIter* props_iter, const std::string& default_addr) {

    DeviceInfo device;
    device.addr = default_addr;      // 蓝牙地址 (BDADDR)
    device.port = Defaults::SIGNALING_CHANNEL;  // 信令 RFCOMM 通道号
    device.manual = false;           // 自动发现的设备

    // 遍历属性字典 { "属性名" → variant(值) }
    while (dbus_message_iter_get_arg_type(props_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter prop_entry, prop_value;
        dbus_message_iter_recurse(props_iter, &prop_entry);

        // 属性名 (STRING)
        const char* prop_name = nullptr;
        dbus_message_iter_get_basic(&prop_entry, &prop_name);

        if (!prop_name) {
            dbus_message_iter_next(props_iter);
            continue;
        }

        // 属性值 (VARIANT)
        if (!dbus_message_iter_next(&prop_entry)) {
            dbus_message_iter_next(props_iter);
            continue;
        }

        dbus_message_iter_recurse(&prop_entry, &prop_value);
        int arg_type = dbus_message_iter_get_arg_type(&prop_value);

        // == Address: BDADDR 字符串 ==
        if (strcmp(prop_name, "Address") == 0 && arg_type == DBUS_TYPE_STRING) {
            const char* val = nullptr;
            dbus_message_iter_get_basic(&prop_value, &val);
            if (val) device.addr = val;
        }
        // == Name: 设备显示名称 ==
        else if (strcmp(prop_name, "Name") == 0 && arg_type == DBUS_TYPE_STRING) {
            const char* val = nullptr;
            dbus_message_iter_get_basic(&prop_value, &val);
            if (val) device.name = val;
        }
        // == Alias: 设备别名 (优先级高于 Name) ==
        else if (strcmp(prop_name, "Alias") == 0 && arg_type == DBUS_TYPE_STRING) {
            const char* val = nullptr;
            dbus_message_iter_get_basic(&prop_value, &val);
            if (val && device.name.empty()) device.name = val;
        }
        // == RSSI: 信号强度 (int16) ==
        else if (strcmp(prop_name, "RSSI") == 0 && arg_type == DBUS_TYPE_INT16) {
            dbus_int16_t rssi = 0;
            dbus_message_iter_get_basic(&prop_value, &rssi);
            // RSSI 暂不存入 DeviceInfo (可用于排序)
        }

        dbus_message_iter_next(props_iter);
    }

    // 如果无名称, 使用 BDADDR 作为名称
    if (device.name.empty()) {
        device.name = device.addr;
    }

    return device;
}
