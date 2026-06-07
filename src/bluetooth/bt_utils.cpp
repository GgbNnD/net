// ============================================================
// 蓝牙工具层 - 实现
// 功能: D-Bus 连接 / BDADDR 转换 / RFCOMM Socket 操作
// ============================================================

#include "bluetooth/bt_utils.h"
#include "common/utils.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

// BlueZ 服务名
static const char* BLUEZ_SERVICE   = "org.bluez";
static const char* ADAPTER_IFACE   = "org.bluez.Adapter1";
static const char* AGENT_MANAGER_IFACE = "org.bluez.AgentManager1";
static const char* OBJECT_MANAGER_IFACE = "org.freedesktop.DBus.ObjectManager";
static const char* PROPERTIES_IFACE = "org.freedesktop.DBus.Properties";

// 配对 Agent 对象路径
static const char* AGENT_PATH = "/p2p/bt/agent";

namespace BtUtils {

// ============================================================
// D-Bus 连接管理
// ============================================================

bool init_dbus(DBusConnection*& conn) {
    DBusError error;
    dbus_error_init(&error);

    // 连接到系统总线 (BlueZ 使用系统总线)
    conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
    if (dbus_error_is_set(&error)) {
        std::cerr << "[蓝牙] D-Bus 连接失败: " << error.message << std::endl;
        dbus_error_free(&error);
        return false;
    }

    if (!conn) {
        std::cerr << "[蓝牙] D-Bus 连接返回空指针" << std::endl;
        return false;
    }

    // 设置不自动退出 (避免 daemon 重启导致连接丢失)
    dbus_connection_set_exit_on_disconnect(conn, false);

    std::cout << "[蓝牙] D-Bus 系统总线连接成功" << std::endl;
    return true;
}

void cleanup_dbus(DBusConnection* conn) {
    if (conn) {
        dbus_connection_flush(conn);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
    }
}

bool dispatch_dbus(DBusConnection* conn, int timeout_ms) {
    if (!conn) return false;

    if (timeout_ms > 0) {
        dbus_connection_read_write_dispatch(conn, timeout_ms);
    } else {
        dbus_connection_read_write_dispatch(conn, 0);
    }

    // 返回是否还有待处理消息
    return dbus_connection_get_dispatch_status(conn) == DBUS_DISPATCH_DATA_REMAINS;
}

// ============================================================
// BDADDR 转换
// ============================================================

std::string bdaddr_to_str(const bdaddr_t& bdaddr) {
    char buf[18];  // "AA:BB:CC:DD:EE:FF\0"
    ba2str(&bdaddr, buf);
    return std::string(buf);
}

bool str_to_bdaddr(const std::string& str, bdaddr_t& bdaddr) {
    if (str.size() != 17) return false;
    return str2ba(str.c_str(), &bdaddr) == 0;
}

// ============================================================
// 本机蓝牙适配器信息
// ============================================================

std::string get_local_bdaddr() {
    // 方法1: 通过 socket ioctl 获取 hci0 的地址
    // 创建蓝牙socket用于 ioctl
    int sock = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (sock < 0) {
        // 方法2: 通过读取 /sys/class/bluetooth/hci0/address
        std::ifstream f("/sys/class/bluetooth/hci0/address");
        if (f.is_open()) {
            std::string addr;
            std::getline(f, addr);
            if (addr.size() == 17) return addr;
        }
        return "";
    }

    // 通过 ioctl 获取设备信息
    struct hci_dev_info di;
    memset(&di, 0, sizeof(di));
    di.dev_id = 0;  // hci0

    if (ioctl(sock, HCIGETDEVINFO, (void*)&di) < 0) {
        close(sock);
        return "";
    }
    close(sock);

    return bdaddr_to_str(di.bdaddr);
}

std::string get_local_device_name() {
    char name[248] = {0};

    // 方法1: 通过 ioctl 获取
    int sock = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (sock >= 0) {
        struct hci_dev_info di;
        memset(&di, 0, sizeof(di));
        di.dev_id = 0;
        if (ioctl(sock, HCIGETDEVINFO, (void*)&di) >= 0) {
            strncpy(name, di.name, sizeof(name) - 1);
        }
        close(sock);
    }

    if (strlen(name) > 0) return std::string(name);

    // 方法2: 退回到主机名
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        return std::string(hostname);
    }

    return "P2P-BT-Device";
}

// ============================================================
// BlueZ D-Bus 调用辅助
// ============================================================

std::string get_default_adapter_path(DBusConnection* conn) {
    if (!conn) return "";

    // 调用 ObjectManager.GetManagedObjects
    DBusMessage* reply = get_managed_objects(conn);
    if (!reply) return "";

    std::string adapter_path;
    DBusMessageIter iter, array_iter;
    dbus_message_iter_init(reply, &iter);

    // GetManagedObjects 返回 a{oa{sa{sv}}}
    // 即: 字典 { 对象路径 → 字典 { 接口名 → 字典 { 属性名 → 变量值 } } }
    if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
        dbus_message_iter_recurse(&iter, &array_iter);

        while (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry_iter, interfaces_iter;
            dbus_message_iter_recurse(&array_iter, &entry_iter);

            // 第一个元素: 对象路径 (字符串)
            const char* obj_path = nullptr;
            dbus_message_iter_get_basic(&entry_iter, &obj_path);

            // 第二个元素: 接口字典
            dbus_message_iter_next(&entry_iter);
            dbus_message_iter_recurse(&entry_iter, &interfaces_iter);

            // 遍历接口字典, 查找 Adapter1
            while (dbus_message_iter_get_arg_type(&interfaces_iter) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter iface_entry;
                dbus_message_iter_recurse(&interfaces_iter, &iface_entry);

                const char* iface_name = nullptr;
                dbus_message_iter_get_basic(&iface_entry, &iface_name);

                if (iface_name && strcmp(iface_name, ADAPTER_IFACE) == 0) {
                    adapter_path = obj_path;
                    break;
                }

                dbus_message_iter_next(&interfaces_iter);
            }

            if (!adapter_path.empty()) break;
            dbus_message_iter_next(&array_iter);
        }
    }

    dbus_message_unref(reply);

    if (!adapter_path.empty()) {
        std::cout << "[蓝牙] 找到适配器: " << adapter_path << std::endl;
    } else {
        std::cerr << "[蓝牙] 未找到蓝牙适配器" << std::endl;
    }

    return adapter_path;
}

DBusMessage* get_managed_objects(DBusConnection* conn) {
    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, "/",
        OBJECT_MANAGER_IFACE, "GetManagedObjects");

    if (!msg) return nullptr;

    DBusError error;
    dbus_error_init(&error);

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        conn, msg, 5000, &error);

    dbus_message_unref(msg);

    if (dbus_error_is_set(&error)) {
        std::cerr << "[蓝牙] GetManagedObjects 失败: " << error.message << std::endl;
        dbus_error_free(&error);
        return nullptr;
    }

    return reply;
}

bool set_adapter_property(DBusConnection* conn,
                          const std::string& adapter_path,
                          const std::string& property,
                          bool value) {
    if (!conn || adapter_path.empty()) return false;

    // 构造 Properties.Set 方法调用
    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, adapter_path.c_str(),
        PROPERTIES_IFACE, "Set");

    if (!msg) return false;

    DBusMessageIter iter;
    dbus_message_iter_init_append(msg, &iter);

    // 参数1: 接口名称 (STRING)
    const char* iface = ADAPTER_IFACE;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface);

    // 参数2: 属性名称 (STRING)
    const char* prop = property.c_str();
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &prop);

    // 参数3: 属性值 (VARIANT)
    DBusMessageIter variant;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
                                      DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
    dbus_bool_t val = value ? TRUE : FALSE;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
    dbus_message_iter_close_container(&iter, &variant);

    // 发送并等待回复
    DBusError error;
    dbus_error_init(&error);

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        conn, msg, 3000, &error);

    dbus_message_unref(msg);

    bool ok = true;
    if (dbus_error_is_set(&error)) {
        std::cerr << "[蓝牙] 设置属性 " << property << " 失败: "
                  << error.message << std::endl;
        dbus_error_free(&error);
        ok = false;
    }

    if (reply) dbus_message_unref(reply);
    return ok;
}

bool start_discovery(DBusConnection* conn, const std::string& adapter_path) {
    if (!conn || adapter_path.empty()) return false;

    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, adapter_path.c_str(),
        ADAPTER_IFACE, "StartDiscovery");

    if (!msg) return false;

    DBusError error;
    dbus_error_init(&error);

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        conn, msg, 5000, &error);

    dbus_message_unref(msg);

    if (dbus_error_is_set(&error)) {
        std::cerr << "[蓝牙] StartDiscovery 失败: " << error.message << std::endl;
        dbus_error_free(&error);
        if (reply) dbus_message_unref(reply);
        return false;
    }

    if (reply) dbus_message_unref(reply);
    std::cout << "[蓝牙] 设备扫描已启动" << std::endl;
    return true;
}

bool stop_discovery(DBusConnection* conn, const std::string& adapter_path) {
    if (!conn || adapter_path.empty()) return false;

    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, adapter_path.c_str(),
        ADAPTER_IFACE, "StopDiscovery");

    if (!msg) return false;

    DBusError error;
    dbus_error_init(&error);

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        conn, msg, 3000, &error);

    dbus_message_unref(msg);

    if (dbus_error_is_set(&error)) {
        dbus_error_free(&error);
        if (reply) dbus_message_unref(reply);
        return false;
    }

    if (reply) dbus_message_unref(reply);
    std::cout << "[蓝牙] 设备扫描已停止" << std::endl;
    return true;
}

void check_dbus_error(DBusError& error, const char* context) {
    if (dbus_error_is_set(&error)) {
        std::cerr << "[蓝牙] " << context << ": " << error.message << std::endl;
        dbus_error_free(&error);
    }
}

// ============================================================
// D-Bus 信号过滤器
// ============================================================

bool add_dbus_filter(DBusConnection* conn,
                     DBusHandleMessageFunction filter,
                     void* user_data) {
    if (!conn || !filter) return false;

    if (!dbus_connection_add_filter(conn, filter, user_data, nullptr)) {
        std::cerr << "[蓝牙] 添加 D-Bus 过滤器失败" << std::endl;
        return false;
    }

    // 添加 match rule 以接收 InterfacesAdded 和 PropertiesChanged 信号
    DBusError error;
    dbus_error_init(&error);

    dbus_bus_add_match(conn,
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager'",
        &error);
    check_dbus_error(error, "添加 ObjectManager match");

    dbus_bus_add_match(conn,
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties'",
        &error);
    check_dbus_error(error, "添加 Properties match");

    dbus_connection_flush(conn);
    return true;
}

void remove_dbus_filter(DBusConnection* conn,
                        DBusHandleMessageFunction filter,
                        void* user_data) {
    if (!conn || !filter) return;

    dbus_connection_remove_filter(conn, filter, user_data);

    // 移除 match rules
    DBusError error;
    dbus_error_init(&error);
    dbus_bus_remove_match(conn,
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager'",
        &error);
    dbus_bus_remove_match(conn,
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties'",
        &error);
    dbus_connection_flush(conn);
}

// ============================================================
// 自动配对 Agent (NoInputNoOutput)
// ============================================================

/**
 * D-Bus 消息处理器: 响应 BlueZ Agent1 方法调用
 *
 * BlueZ 配对过程会依次调用 Agent 的以下方法:
 * 1. RequestPinCode(device) → STRING   -- 返回空字符串 (NoInputNoOutput)
 * 2. DisplayPinCode(device, pincode) → void -- 仅确认
 * 3. RequestConfirmation(device, passkey) → void -- 自动接受
 * 4. RequestAuthorization(device) → void -- 自动接受
 * 5. AuthorizeService(device, uuid) → void -- 自动接受
 * 6. Cancel() → void -- 取消通知
 *
 * 对于 NoInputNoOutput 模式, 所有请求都被自动接受
 */
DBusHandlerResult handle_agent_message(DBusConnection* conn,
                                        DBusMessage* msg,
                                        void* /*user_data*/) {
    // 只处理方法调用 (忽略信号和返回值)
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    // 只处理目标为我们的 Agent 路径的消息
    const char* path = dbus_message_get_path(msg);
    if (!path || strcmp(path, AGENT_PATH) != 0) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char* iface = dbus_message_get_interface(msg);
    if (!iface || strcmp(iface, "org.bluez.Agent1") != 0) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char* method = dbus_message_get_member(msg);
    if (!method) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    std::cout << "[蓝牙] Agent 方法: " << method << std::endl;

    // == RequestPinCode: 返回空 PIN (NoInputNoOutput) ==
    if (strcmp(method, "RequestPinCode") == 0) {
        const char* pin = "";
        dbus_message_append_args(reply,
            DBUS_TYPE_STRING, &pin,
            DBUS_TYPE_INVALID);
    }
    // == DisplayPinCode: 仅确认, 无返回值 ==
    else if (strcmp(method, "DisplayPinCode") == 0) {
        // void return, 无需附加参数
    }
    // == RequestPasskey: 返回 passkey 0 ==
    else if (strcmp(method, "RequestPasskey") == 0) {
        dbus_uint32_t passkey = 0;
        dbus_message_append_args(reply,
            DBUS_TYPE_UINT32, &passkey,
            DBUS_TYPE_INVALID);
    }
    // == DisplayPasskey / RequestConfirmation / RequestAuthorization /
    //    AuthorizeService / Cancel: 自动接受, 无返回值 ==
    else if (strcmp(method, "RequestConfirmation") == 0 ||
             strcmp(method, "RequestAuthorization") == 0 ||
             strcmp(method, "AuthorizeService") == 0 ||
             strcmp(method, "DisplayPasskey") == 0 ||
             strcmp(method, "Cancel") == 0) {
        // void return
    }
    else {
        // 未知方法, 不处理
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    // 发送回复
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);

    return DBUS_HANDLER_RESULT_HANDLED;
}

std::string register_auto_pair_agent(DBusConnection* conn,
                                      const std::string& adapter_path) {
    if (!conn || adapter_path.empty()) return "";

    // 使用固定的 agent 路径
    const char* agent_path = AGENT_PATH;

    // AgentManager1 位于 BlueZ 根路径 "/org/bluez", 而非适配器路径
    const char* mgr_path = "/org/bluez";

    // 1. 注册 Agent
    {
        DBusMessage* msg = dbus_message_new_method_call(
            BLUEZ_SERVICE, mgr_path,
            AGENT_MANAGER_IFACE, "RegisterAgent");

        if (!msg) return "";

        DBusMessageIter iter;
        dbus_message_iter_init_append(msg, &iter);

        // 参数1: agent 对象路径 (OBJECT_PATH)
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &agent_path);

        // 参数2: 能力 (STRING) - "NoInputNoOutput"
        const char* capability = "NoInputNoOutput";
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &capability);

        DBusError error;
        dbus_error_init(&error);

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(
            conn, msg, 3000, &error);

        dbus_message_unref(msg);

        if (dbus_error_is_set(&error)) {
            std::cerr << "[蓝牙] 注册 Agent 失败: " << error.message << std::endl;
            dbus_error_free(&error);
            if (reply) dbus_message_unref(reply);
            return "";
        }
        if (reply) dbus_message_unref(reply);
    }

    // 2. 设置为默认 Agent
    {
        DBusMessage* msg = dbus_message_new_method_call(
            BLUEZ_SERVICE, mgr_path,
            AGENT_MANAGER_IFACE, "RequestDefaultAgent");

        if (!msg) return "";

        DBusMessageIter iter;
        dbus_message_iter_init_append(msg, &iter);

        dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &agent_path);

        DBusError error;
        dbus_error_init(&error);

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(
            conn, msg, 3000, &error);

        dbus_message_unref(msg);

        if (dbus_error_is_set(&error)) {
            std::cerr << "[蓝牙] 设置默认 Agent 失败: " << error.message << std::endl;
            dbus_error_free(&error);
            if (reply) dbus_message_unref(reply);
            // 仍然返回 agent_path, Agent 已注册但非默认
        }
        if (reply) dbus_message_unref(reply);
    }

    std::cout << "[蓝牙] 自动配对 Agent 已注册 (NoInputNoOutput)" << std::endl;
    return agent_path;
}

bool unregister_auto_pair_agent(DBusConnection* conn,
                                 const std::string& /*adapter_path*/,
                                 const std::string& agent_path) {
    if (!conn || agent_path.empty()) return false;

    // AgentManager1 位于 BlueZ 根路径
    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, "/org/bluez",
        AGENT_MANAGER_IFACE, "UnregisterAgent");

    if (!msg) return false;

    const char* ap = agent_path.c_str();
    dbus_message_append_args(msg,
        DBUS_TYPE_OBJECT_PATH, &ap,
        DBUS_TYPE_INVALID);

    DBusError error;
    dbus_error_init(&error);

    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        conn, msg, 3000, &error);

    dbus_message_unref(msg);

    if (dbus_error_is_set(&error)) {
        std::cerr << "[蓝牙] 取消注册 Agent 失败: " << error.message << std::endl;
        dbus_error_free(&error);
        if (reply) dbus_message_unref(reply);
        return false;
    }

    if (reply) dbus_message_unref(reply);
    return true;
}

// ============================================================
// RFCOMM Socket 辅助函数
// ============================================================

SOCKET_FD create_rfcomm_server(uint8_t channel, int backlog) {
    // 1. 创建 RFCOMM Socket
    SOCKET_FD sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (sock < 0) {
        std::cerr << "[蓝牙] 创建 RFCOMM Socket 失败: "
                  << strerror(errno) << std::endl;
        return INVALID_SOCKET_FD;
    }

    // 2. 绑定到任意本机蓝牙适配器 + 指定通道
    struct sockaddr_rc addr;
    memset(&addr, 0, sizeof(addr));
    addr.rc_family  = AF_BLUETOOTH;
    addr.rc_channel = channel;
    // bdaddr_any: 00:00:00:00:00:00 → 绑定到任意本机蓝牙适配器
    bdaddr_t bdaddr_any = {0};
    bacpy(&addr.rc_bdaddr, &bdaddr_any);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[蓝牙] RFCOMM bind() 失败 (通道 " << (int)channel
                  << "): " << strerror(errno) << std::endl;
        close(sock);
        return INVALID_SOCKET_FD;
    }

    // 3. 开始监听
    if (listen(sock, backlog) < 0) {
        std::cerr << "[蓝牙] RFCOMM listen() 失败: "
                  << strerror(errno) << std::endl;
        close(sock);
        return INVALID_SOCKET_FD;
    }

    std::cout << "[蓝牙] RFCOMM 服务端已启动 (通道 " << (int)channel
              << ", backlog=" << backlog << ")" << std::endl;
    return sock;
}

SOCKET_FD rfcomm_connect(const std::string& target_addr,
                         uint8_t channel,
                         uint32_t timeout_ms) {
    // 1. 解析目标蓝牙地址
    bdaddr_t bdaddr;
    if (!str_to_bdaddr(target_addr, bdaddr)) {
        std::cerr << "[蓝牙] 无效的蓝牙地址: " << target_addr << std::endl;
        return INVALID_SOCKET_FD;
    }

    // 2. 创建 RFCOMM Socket
    SOCKET_FD sock = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
    if (sock < 0) {
        std::cerr << "[蓝牙] 创建 RFCOMM Socket 失败: "
                  << strerror(errno) << std::endl;
        return INVALID_SOCKET_FD;
    }

    // 3. 构造目标地址
    struct sockaddr_rc addr;
    memset(&addr, 0, sizeof(addr));
    addr.rc_family  = AF_BLUETOOTH;
    addr.rc_channel = channel;
    bacpy(&addr.rc_bdaddr, &bdaddr);

    // 4. 非阻塞连接 (带超时)
    // 先设为非阻塞
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::cerr << "[蓝牙] 设置非阻塞模式失败" << std::endl;
        close(sock);
        return INVALID_SOCKET_FD;
    }

    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        std::cerr << "[蓝牙] connect() 失败: " << strerror(errno) << std::endl;
        close(sock);
        return INVALID_SOCKET_FD;
    }

    if (ret == 0) {
        // 连接立即成功 (罕见)
        fcntl(sock, F_SETFL, flags);  // 恢复阻塞模式
        return sock;
    }

    // 5. 使用 select() 等待连接完成
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int sel = select(sock + 1, nullptr, &write_fds, nullptr, &tv);

    if (sel <= 0) {
        if (sel == 0) {
            std::cerr << "[蓝牙] 连接超时 (" << timeout_ms
                      << "ms) → " << target_addr << std::endl;
        } else {
            std::cerr << "[蓝牙] select() 错误: " << strerror(errno) << std::endl;
        }
        close(sock);
        return INVALID_SOCKET_FD;
    }

    // 6. 检查 SO_ERROR 确认连接成功
    int so_error = 0;
    socklen_t so_len = sizeof(so_error);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &so_len) < 0
        || so_error != 0) {
        std::cerr << "[蓝牙] 连接失败: " << strerror(so_error) << std::endl;
        close(sock);
        return INVALID_SOCKET_FD;
    }

    // 7. 恢复阻塞模式
    fcntl(sock, F_SETFL, flags);

    std::cout << "[蓝牙] 已连接: " << target_addr
              << " (通道 " << (int)channel << ")" << std::endl;
    return sock;
}

} // namespace BtUtils
