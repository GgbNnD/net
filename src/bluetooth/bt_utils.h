#pragma once

// ============================================================
// 蓝牙工具层 - D-Bus连接 / BDADDR转换 / RFCOMM Socket辅助
// 功能: 提供蓝牙通信所需的基础工具函数
// 依赖: libbluetooth (AF_BLUETOOTH), libdbus-1 (BlueZ D-Bus API)
// Linux 平台专用
// ============================================================

#include <string>
#include <cstdint>
#include <dbus/dbus.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#include "common/platform.h"

namespace BtUtils {

// ----------------------------------------------------------
// D-Bus 系统总线连接
// ----------------------------------------------------------

/**
 * @brief 连接到 D-Bus 系统总线并启动 BlueZ 交互
 * @param conn  输出: D-Bus 连接句柄
 * @return 成功返回 true, 失败返回 false
 *
 * 使用 dbus_bus_get_private() 获取独占连接,
 * 避免与其他使用 D-Bus 的线程冲突
 */
bool init_dbus(DBusConnection*& conn);

/**
 * @brief 释放 D-Bus 连接, 关闭并释放资源
 */
void cleanup_dbus(DBusConnection* conn);

/**
 * @brief 分发 D-Bus 待处理消息 (非阻塞轮询)
 * @param conn       D-Bus 连接句柄
 * @param timeout_ms 最大等待时间(毫秒), 0 = 立即返回
 * @return 是否还有待处理消息
 *
 * 需要在独立线程中周期性调用, 确保 incoming signals 被处理
 */
bool dispatch_dbus(DBusConnection* conn, int timeout_ms = 0);

// ----------------------------------------------------------
// BDADDR 字符串/结构体互转
// ----------------------------------------------------------

/**
 * @brief 蓝牙地址结构体 → 字符串 "AA:BB:CC:DD:EE:FF"
 */
std::string bdaddr_to_str(const bdaddr_t& bdaddr);

/**
 * @brief 字符串 "AA:BB:CC:DD:EE:FF" → 蓝牙地址结构体
 */
bool str_to_bdaddr(const std::string& str, bdaddr_t& bdaddr);

/**
 * @brief 比较两个蓝牙地址是否相等
 */
inline bool bdaddr_equal(const bdaddr_t& a, const bdaddr_t& b) {
    return bacmp(&a, &b) == 0;
}

// ----------------------------------------------------------
// 本机蓝牙适配器信息
// ----------------------------------------------------------

/**
 * @brief 获取本机蓝牙适配器地址
 * 通过 ioctl HCIGETDEVINFO 或 BlueZ D-Bus 获取
 */
std::string get_local_bdaddr();

/**
 * @brief 获取本机蓝牙适配器名称 (Alias)
 */
std::string get_local_device_name();

// ----------------------------------------------------------
// BlueZ D-Bus 调用辅助函数
// ----------------------------------------------------------

/**
 * @brief 获取默认 BlueZ 适配器的 D-Bus 对象路径
 * 通过 ObjectManager.GetManagedObjects 找到第一个 Adapter1 接口
 * @return 适配器路径 (如 "/org/bluez/hci0"), 失败返回空字符串
 */
std::string get_default_adapter_path(DBusConnection* conn);

/**
 * @brief 设置 BlueZ 适配器的布尔属性 (如 Discoverable, Pairable)
 * @param conn         D-Bus 连接
 * @param adapter_path 适配器路径
 * @param property     属性名称
 * @param value        新值
 * @return 成功返回 true
 */
bool set_adapter_property(DBusConnection* conn,
                          const std::string& adapter_path,
                          const std::string& property,
                          bool value);

/**
 * @brief 调用 BlueZ ObjectManager.GetManagedObjects
 * @param conn D-Bus 连接
 * @return D-Bus 方法调用的回复消息, 调用者需要 dbus_message_unref()
 *         失败返回 nullptr
 *
 * GetManagedObjects 返回一个字典:
 *   { "/org/bluez/hci0/dev_XX_XX_XX": {
 *       "org.bluez.Device1": { "Address": "AA:BB:...", "Name": "...", ... }
 *     }
 *   }
 */
DBusMessage* get_managed_objects(DBusConnection* conn);

/**
 * @brief 调用 BlueZ Adapter.StartDiscovery / StopDiscovery
 */
bool start_discovery(DBusConnection* conn, const std::string& adapter_path);
bool stop_discovery(DBusConnection* conn, const std::string& adapter_path);

/**
 * @brief 检查 D-Bus 错误并打印, 清空 error
 */
void check_dbus_error(DBusError& error, const char* context);

// ----------------------------------------------------------
// D-Bus 信号过滤器 (用于监听 BlueZ 事件)
// ----------------------------------------------------------

/**
 * @brief 添加 D-Bus 消息过滤器 (监听 InterfacesAdded / PropertiesChanged)
 * @param conn    D-Bus 连接
 * @param filter  过滤器回调
 * @param user_data 回调用户数据
 * @param rule    D-Bus match rule (nullptr = 默认)
 */
bool add_dbus_filter(DBusConnection* conn,
                     DBusHandleMessageFunction filter,
                     void* user_data);

void remove_dbus_filter(DBusConnection* conn,
                        DBusHandleMessageFunction filter,
                        void* user_data);

// ----------------------------------------------------------
// 自动配对 Agent (NoInputNoOutput)
// ----------------------------------------------------------

/**
 * @brief 注册自动配对 Agent (NoInputNoOutput)
 *
 * 实现最简配对策略: 对所有配对请求自动接受,
 * 无需用户交互 (PIN / Passkey 确认)
 *
 * @param conn         D-Bus 连接
 * @param adapter_path 适配器路径
 * @return 自定义 agent 对象路径 或 空字符串
 */
std::string register_auto_pair_agent(DBusConnection* conn,
                                     const std::string& adapter_path);

/**
 * @brief 取消注册 Agent
 */
bool unregister_auto_pair_agent(DBusConnection* conn,
                                 const std::string& adapter_path,
                                 const std::string& agent_path);

/**
 * @brief Agent 消息处理函数 (被 D-Bus 过滤器调用)
 *
 * 处理来自 BlueZ 的 Agent1 方法调用:
 *   - org.bluez.Agent1.RequestPinCode
 *   - org.bluez.Agent1.DisplayPinCode
 *   - org.bluez.Agent1.RequestConfirmation
 *   - org.bluez.Agent1.RequestAuthorization
 *   - org.bluez.Agent1.AuthorizeService
 *   - org.bluez.Agent1.Cancel
 *
 * @param conn   D-Bus 连接
 * @param msg    收到的 D-Bus 消息
 * @param user_data 用户数据 (未使用)
 * @return DBUS_HANDLER_RESULT_HANDLED 或 DBUS_HANDLER_RESULT_NOT_YET_HANDLED
 */
DBusHandlerResult handle_agent_message(DBusConnection* conn,
                                        DBusMessage* msg,
                                        void* user_data);

// ----------------------------------------------------------
// RFCOMM Socket 辅助函数
// ----------------------------------------------------------

/**
 * @brief 创建 RFCOMM 服务端 Socket
 * @param channel RFCOMM 通道号 (1-30)
 * @param backlog listen 队列长度
 * @return Socket 描述符, 失败返回 INVALID_SOCKET_FD
 *
 * 流程: socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM)
 *        → bind(BDADDR_ANY, channel)
 *        → listen(backlog)
 */
SOCKET_FD create_rfcomm_server(uint8_t channel, int backlog = 1);

/**
 * @brief 创建 RFCOMM 客户端 Socket 并连接到目标设备
 * @param target_addr 目标蓝牙地址 "AA:BB:CC:DD:EE:FF"
 * @param channel     RFCOMM 通道号
 * @param timeout_ms  连接超时 (毫秒), 使用非阻塞 connect + select
 * @return Socket 描述符, 失败返回 INVALID_SOCKET_FD
 */
SOCKET_FD rfcomm_connect(const std::string& target_addr,
                         uint8_t channel,
                         uint32_t timeout_ms = 5000);

} // namespace BtUtils
