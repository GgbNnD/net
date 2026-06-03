// ============================================================
// P2P局域网文件传输工具 - 程序入口
// 功能: 初始化各个模块, 启动Web服务器, 进入主循环
// ============================================================

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include "common/platform.h"
#include "common/utils.h"
#include "common/types.h"
#include "common/protocol.h"

// ----------------------------------------------------------
// 全局标志: 用于优雅退出
// ----------------------------------------------------------
static std::atomic<bool> g_running(true);

/**
 * @brief 信号处理函数
 * 捕获 Ctrl+C (SIGINT) 或终止信号, 触发优雅退出流程
 */
void signal_handler(int signal) {
    std::cout << "\n[系统] 收到信号 " << signal << ", 正在退出..." << std::endl;
    g_running = false;
}

/**
 * @brief 打印程序启动信息
 */
void print_banner() {
    std::cout << "============================================" << std::endl;
    std::cout << "  P2P局域网文件传输工具 v1.0.0" << std::endl;
    std::cout << "  P2P LAN File Transfer Tool" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << std::endl;
}

/**
 * @brief 测试工具函数的单元验证
 */
void run_unit_tests() {
    std::cout << "=== 单元验证: 工具函数 ===" << std::endl;

    // 测试UUID生成
    std::string uuid = Utils::generate_uuid();
    std::cout << "  [OK] UUID生成: " << uuid << " (长度=" << uuid.size() << ")" << std::endl;

    // 测试MD5
    const char* test_str = "Hello, P2P!";
    std::string hash = Utils::md5_data(reinterpret_cast<const uint8_t*>(test_str),
                                        strlen(test_str));
    std::cout << "  [OK] MD5(\"Hello, P2P!\"): " << hash << std::endl;

    // 测试格式化函数
    std::cout << "  [OK] 文件大小格式化: " << Utils::format_file_size(10485760) << std::endl;
    std::cout << "  [OK] 速度格式化: " << Utils::format_speed(5242880) << std::endl;
    std::cout << "  [OK] 当前时间: " << Utils::get_time_string() << std::endl;
    std::cout << "  [OK] 时间戳(ms): " << Utils::get_timestamp_ms() << std::endl;

    // 测试协议消息构建
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

    // 测试文件名提取
    std::cout << "  [OK] 文件名: " << Utils::get_filename("/home/user/Documents/test.pdf") << std::endl;

    std::cout << "=== 单元验证完成 ===" << std::endl;
    std::cout << std::endl;
}

/**
 * @brief 程序入口
 */
int main() {
    // 打印启动横幅
    print_banner();

    // 注册信号处理函数 (Ctrl+C 优雅退出)
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 初始化网络库 (Windows WSAStartup / Linux 无操作)
    if (!NetworkUtils::initialize()) {
        std::cerr << "[错误] 网络库初始化失败!" << std::endl;
        return 1;
    }
    std::cout << "[系统] 网络库初始化成功" << std::endl;

    // 显示本机IP地址
    auto ips = NetworkUtils::get_local_ips();
    std::cout << "[系统] 本机IP地址: ";
    for (size_t i = 0; i < ips.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << ips[i];
    }
    std::cout << std::endl << std::endl;

    // 运行单元验证
    run_unit_tests();

    // ----------------------------------------------------------
    // TODO: 后续阶段将在此处初始化各个模块
    // - 设备发现模块 (UDP组播)
    // - 信令控制模块 (TCP服务端/客户端)
    // - 文件传输模块 (TCP + 滑动窗口)
    // - Web界面模块 (HTTP服务器)
    // ----------------------------------------------------------

    std::cout << "[系统] 程序启动完成, 按 Ctrl+C 退出" << std::endl;

    // 主循环: 等待退出信号
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 清理网络库
    NetworkUtils::cleanup();
    std::cout << "[系统] 程序已退出" << std::endl;

    return 0;
}
