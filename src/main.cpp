// ============================================================
// P2P局域网文件传输工具 - 程序入口
// 功能: 初始化各个模块, 启动Web服务器, 进入主循环
// ============================================================

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <memory>
#include <iomanip>
#include "common/platform.h"
#include "common/utils.h"
#include "common/types.h"
#include "common/protocol.h"
#include "discovery/device_discovery.h"
#include "discovery/device_manager.h"

// ----------------------------------------------------------
// 全局变量
// ----------------------------------------------------------
static std::atomic<bool> g_running(true);
static std::unique_ptr<DeviceDiscovery> g_discovery;
static std::unique_ptr<DeviceManager>   g_device_manager;
static std::string g_device_id;   // 本机设备ID
static std::string g_device_name; // 本机设备名称

/**
 * @brief 信号处理函数
 * 捕获 Ctrl+C (SIGINT) 或终止信号, 触发优雅退出流程
 */
void signal_handler(int signal) {
    std::cout << "\n[系统] 收到信号 " << signal << ", 正在退出..." << std::endl;
    g_running = false;
}

/**
 * @brief 打印程序启动横幅
 */
void print_banner() {
    std::cout << "============================================" << std::endl;
    std::cout << "  P2P局域网文件传输工具 v1.0.0" << std::endl;
    std::cout << "  P2P LAN File Transfer Tool" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << std::endl;
}

/**
 * @brief 生成人类可读的设备名称
 * 格式: "User-XXXX" (XXXX = 设备ID前8位)
 */
std::string generate_device_name() {
    // 尝试获取主机名作为设备名称的一部分
    std::string hostname = "P2P-Device";
#ifdef _WIN32
    char buf[256];
    DWORD size = sizeof(buf);
    if (GetComputerNameA(buf, &size)) {
        hostname = buf;
    }
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        hostname = buf;
    }
#endif
    // 截断过长的hostname
    if (hostname.size() > 20) {
        hostname = hostname.substr(0, 20);
    }
    return hostname;
}

/**
 * @brief 阶段1: 基础工具函数单元验证
 */
void run_phase1_tests() {
    std::cout << "=== 单元验证: 工具函数 ===" << std::endl;

    std::string uuid = Utils::generate_uuid();
    std::cout << "  [OK] UUID生成: " << uuid << " (长度=" << uuid.size() << ")" << std::endl;

    const char* test_str = "Hello, P2P!";
    std::string hash = Utils::md5_data(reinterpret_cast<const uint8_t*>(test_str),
                                        strlen(test_str));
    std::cout << "  [OK] MD5(\"Hello, P2P!\"): " << hash << std::endl;

    std::cout << "  [OK] 文件大小格式化: " << Utils::format_file_size(10485760) << std::endl;
    std::cout << "  [OK] 速度格式化: " << Utils::format_speed(5242880) << std::endl;
    std::cout << "  [OK] 当前时间: " << Utils::get_time_string() << std::endl;
    std::cout << "  [OK] 时间戳(ms): " << Utils::get_timestamp_ms() << std::endl;

    json msg = Protocol::build_device_broadcast(
        "test-uuid", "TestDevice", "192.168.1.100", 8889,
        Utils::get_timestamp_ms()
    );
    std::cout << "  [OK] 设备广播消息: " << msg.dump() << std::endl;

    json req = Protocol::build_file_request(
        "file-uuid", "test.pdf", 1024000,
        "d41d8cd98f00b204e9800998ecf8427e", 65536, 16
    );
    std::cout << "  [OK] 文件请求消息: " << req.dump() << std::endl;

    std::cout << "  [OK] 文件名提取: " << Utils::get_filename("/home/user/Documents/test.pdf")
              << std::endl;

    std::cout << "=== 单元验证完成 ===" << std::endl << std::endl;
}

/**
 * @brief 阶段2: 设备发现模块单元验证
 *
 * 验证内容:
 * 1. 设备管理器线程安全操作 (增删查)
 * 2. 设备超时自动清理
 * 3. 设备上线/离线回调
 */
void run_phase2_tests() {
    std::cout << "=== 单元验证: 设备发现模块 ===" << std::endl;

    // 测试1: 设备管理器基本操作
    {
        std::cout << "  [测试] 设备管理器增删查..." << std::endl;

        DeviceManager dm("self-id-001", 2);  // 2秒超时 (加速测试)

        // 添加3个测试设备
        DeviceInfo d1; d1.id = "d1"; d1.name = "Device-1"; d1.ip = "192.168.1.101"; d1.port = 8889;
        DeviceInfo d2; d2.id = "d2"; d2.name = "Device-2"; d2.ip = "192.168.1.102"; d2.port = 8889;
        DeviceInfo d3; d3.id = "self-id-001"; d3.name = "Self"; d3.ip = "192.168.1.100"; d3.port = 8889;

        dm.update_device(d1);
        dm.update_device(d2);
        dm.update_device(d3);  // 自己的ID, 应该被忽略

        // 验证数量 (排除自己, 应该只有2个)
        if (dm.get_device_count() == 2) {
            std::cout << "    [OK] 设备数量: 2 (已排除自己)" << std::endl;
        } else {
            std::cout << "    [失败] 预期2, 实际 " << dm.get_device_count() << std::endl;
        }

        // 验证查找
        DeviceInfo found;
        if (dm.find_device("d1", found) && found.name == "Device-1") {
            std::cout << "    [OK] 查找设备 'd1': 成功" << std::endl;
        } else {
            std::cout << "    [失败] 查找设备 'd1' 失败" << std::endl;
        }

        // 验证移除
        dm.remove_device("d1");
        if (dm.get_device_count() == 1) {
            std::cout << "    [OK] 移除设备后数量: 1" << std::endl;
        } else {
            std::cout << "    [失败] 移除后预期1, 实际 " << dm.get_device_count() << std::endl;
        }
    }

    // 测试2: 设备超时自动清理
    {
        std::cout << "  [测试] 设备超时自动清理..." << std::endl;

        DeviceManager dm("self-id", 1);  // 1秒超时

        DeviceInfo d1; d1.id = "d1"; d1.name = "ShortLive"; d1.ip = "10.0.0.1"; d1.port = 8889;
        dm.update_device(d1);

        // 启动管理器 (启动清理线程)
        dm.start();

        // 验证设备存在
        if (dm.get_device_count() == 1) {
            std::cout << "    [OK] 设备已添加" << std::endl;
        }

        // 等待超时 (1秒超时 + 1秒检查间隔 = 约2秒)
        std::this_thread::sleep_for(std::chrono::seconds(3));

        // 验证设备已被清理
        if (dm.get_device_count() == 0) {
            std::cout << "    [OK] 设备已自动清理 (超时)" << std::endl;
        } else {
            std::cout << "    [失败] 预期0, 实际 " << dm.get_device_count()
                      << " (超时未清理)" << std::endl;
        }

        dm.stop();
    }

    // 测试3: 设备管理器上线/离线回调
    {
        std::cout << "  [测试] 设备上线/离线回调..." << std::endl;

        DeviceManager dm("self-id", 10);
        int online_count = 0;
        int offline_count = 0;

        dm.set_on_device_online([&](const DeviceInfo&) { ++online_count; });
        dm.set_on_device_offline([&](const DeviceInfo&) { ++offline_count; });

        DeviceInfo d1; d1.id = "cb-test"; d1.name = "CallbackDevice"; d1.ip = "10.0.0.1"; d1.port = 8889;
        dm.update_device(d1);

        // 验证上线回调
        if (online_count == 1) {
            std::cout << "    [OK] 上线回调触发: " << online_count << " 次" << std::endl;
        } else {
            std::cout << "    [失败] 上线回调预期1次, 实际 " << online_count << " 次" << std::endl;
        }

        dm.remove_device("cb-test");

        // 验证离线回调
        if (offline_count == 1) {
            std::cout << "    [OK] 离线回调触发: " << offline_count << " 次" << std::endl;
        } else {
            std::cout << "    [失败] 离线回调预期1次, 实际 " << offline_count << " 次" << std::endl;
        }
    }

    // 测试4: 获取在线设备列表
    {
        std::cout << "  [测试] 在线设备列表..." << std::endl;

        DeviceManager dm("self", 10);

        DeviceInfo d1; d1.id = "a1"; d1.name = "Device-A"; d1.ip = "10.0.0.1"; d1.port = 8889;
        DeviceInfo d2; d2.id = "a2"; d2.name = "Device-B"; d2.ip = "10.0.0.2"; d2.port = 8889;
        dm.update_device(d1);
        dm.update_device(d2);

        auto devices = dm.get_online_devices();
        if (devices.size() == 2 &&
            ((devices[0].name == "Device-A" && devices[1].name == "Device-B") ||
             (devices[0].name == "Device-B" && devices[1].name == "Device-A"))) {
            std::cout << "    [OK] 列表包含2个设备, 信息正确" << std::endl;
        } else {
            std::cout << "    [失败] 列表内容错误" << std::endl;
        }
    }

    std::cout << "=== 设备发现模块验证完成 ===" << std::endl << std::endl;
}

/**
 * @brief 初始化设备发现服务
 *
 * 创建 DeviceDiscovery 和 DeviceManager 实例,
 * 连接它们的回调函数, 启动服务
 */
bool init_discovery_service() {
    // 1. 生成设备ID和名称
    g_device_id = Utils::generate_uuid();
    g_device_name = generate_device_name();
    std::cout << "[系统] 设备ID:   " << g_device_id << std::endl;
    std::cout << "[系统] 设备名称: " << g_device_name << std::endl;

    // 2. 创建设备管理器
    g_device_manager = std::make_unique<DeviceManager>(g_device_id);
    g_device_manager->set_on_device_online([](const DeviceInfo& device) {
        std::cout << "[事件] 设备上线: " << device.name
                  << " (" << device.ip << ":" << device.port << ")" << std::endl;
    });
    g_device_manager->set_on_device_offline([](const DeviceInfo& device) {
        std::cout << "[事件] 设备离线: " << device.name
                  << " (" << device.ip << ":" << device.port << ")" << std::endl;
    });
    g_device_manager->start();

    // 3. 创建设备发现服务
    g_discovery = std::make_unique<DeviceDiscovery>(
        g_device_id, g_device_name, Defaults::DISCOVERY_PORT
    );

    // 收到其他设备广播 -> 更新设备管理器
    g_discovery->set_on_device_found([](const DeviceInfo& device) {
        g_device_manager->update_device(device);
    });

    // 收到其他设备离线通知 -> 从管理器移除
    g_discovery->set_on_device_offline([](const std::string& device_id) {
        g_device_manager->remove_device(device_id);
    });

    // 4. 启动发现服务
    if (!g_discovery->start()) {
        std::cerr << "[错误] 设备发现服务启动失败" << std::endl;
        return false;
    }

    return true;
}

/**
 * @brief 程序入口
 */
int main() {
    print_banner();

    // 注册信号处理函数
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 初始化网络库
    if (!NetworkUtils::initialize()) {
        std::cerr << "[错误] 网络库初始化失败!" << std::endl;
        return 1;
    }
    std::cout << "[系统] 网络库初始化成功" << std::endl;

    // 显示本机IP
    auto ips = NetworkUtils::get_local_ips();
    std::cout << "[系统] 本机IP地址: ";
    for (size_t i = 0; i < ips.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << ips[i];
    }
    std::cout << std::endl << std::endl;

    // 运行单元验证
    run_phase1_tests();
    run_phase2_tests();

    // 启动设备发现服务
    if (!init_discovery_service()) {
        return 1;
    }

    std::cout << "\n[系统] 程序启动完成, 按 Ctrl+C 退出" << std::endl;

    // 主循环: 定期打印设备列表, 等待退出信号
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        // 定期打印在线设备列表
        if (g_device_manager) {
            auto devices = g_device_manager->get_online_devices();
            if (!devices.empty()) {
                std::cout << "\n[在线设备] (" << devices.size() << " 台):" << std::endl;
                std::cout << "  " << std::left << std::setw(24) << "设备名称"
                          << std::setw(18) << "IP地址"
                          << std::setw(8) << "端口" << std::endl;
                std::cout << "  " << std::string(50, '-') << std::endl;
                for (const auto& d : devices) {
                    std::cout << "  " << std::left << std::setw(24) << d.name
                              << std::setw(18) << d.ip
                              << std::setw(8) << d.port << std::endl;
                }
                std::cout << std::endl;
            } else {
                std::cout << "[在线设备] 当前没有其他设备在线" << std::endl;
            }
        }
    }

    // 优雅退出: 停止发现服务和设备管理器
    std::cout << "\n[系统] 正在停止所有服务..." << std::endl;

    if (g_discovery) {
        g_discovery->stop();
        g_discovery.reset();
    }
    if (g_device_manager) {
        g_device_manager->stop();
        g_device_manager.reset();
    }

    NetworkUtils::cleanup();
    std::cout << "[系统] 程序已退出" << std::endl;

    return 0;
}
