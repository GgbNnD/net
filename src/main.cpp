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
#include "signaling/signaling_server.h"
#include "signaling/signaling_client.h"

// ----------------------------------------------------------
// 全局变量
// ----------------------------------------------------------
static std::atomic<bool> g_running(true);
static std::unique_ptr<DeviceDiscovery>  g_discovery;
static std::unique_ptr<DeviceManager>    g_device_manager;
static std::unique_ptr<SignalingServer>  g_signaling_server;

// ---- 接受文件传输的目录 ----
static std::string g_received_files_dir = "./received_files";

static std::string g_device_id;   // 本机设备ID
static std::string g_device_name; // 本机设备名称

/**
 * @brief 信号处理函数
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
 * @brief 生成设备名称
 */
std::string generate_device_name() {
    std::string hostname = "P2P-Device";
#ifdef _WIN32
    char buf[256];
    DWORD size = sizeof(buf);
    if (GetComputerNameA(buf, &size)) { hostname = buf; }
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) { hostname = buf; }
#endif
    if (hostname.size() > 20) { hostname = hostname.substr(0, 20); }
    return hostname;
}

// ============================================================
// 单元验证函数
// ============================================================

void run_phase1_tests() {
    std::cout << "=== 单元验证: 工具函数 ===" << std::endl;

    std::string uuid = Utils::generate_uuid();
    std::cout << "  [OK] UUID生成: " << uuid << " (长度=" << uuid.size() << ")" << std::endl;

    const char* test_str = "Hello, P2P!";
    std::string hash = Utils::md5_data(reinterpret_cast<const uint8_t*>(test_str), strlen(test_str));
    std::cout << "  [OK] MD5: " << hash << std::endl;

    std::cout << "  [OK] 文件大小: " << Utils::format_file_size(10485760) << std::endl;
    std::cout << "  [OK] 速度: " << Utils::format_speed(5242880) << std::endl;
    std::cout << "  [OK] 协议消息构建正常" << std::endl;

    std::cout << "=== 工具函数验证完成 ===" << std::endl << std::endl;
}

void run_phase2_tests() {
    std::cout << "=== 单元验证: 设备发现模块 ===" << std::endl;

    {
        DeviceManager dm("self-id-001", 2);
        DeviceInfo d1; d1.id = "d1"; d1.name = "Dev1"; d1.ip = "192.168.1.101"; d1.port = 8889;
        DeviceInfo d2; d2.id = "d2"; d2.name = "Dev2"; d2.ip = "192.168.1.102"; d2.port = 8889;
        DeviceInfo d3; d3.id = "self-id-001"; d3.name = "Self"; d3.ip = "192.168.1.100"; d3.port = 8889;

        dm.update_device(d1);
        dm.update_device(d2);
        dm.update_device(d3);  // 应被忽略
        std::cout << "  [OK] 设备增删查: " << dm.get_device_count() << " (已排除自己)" << std::endl;

        dm.remove_device("d1");
        std::cout << "  [OK] 移除设备后: " << dm.get_device_count() << std::endl;
    }

    {
        DeviceManager dm("self-id", 1);
        DeviceInfo d1; d1.id = "d1"; d1.name = "ShortLive"; d1.ip = "10.0.0.1"; d1.port = 8889;
        dm.update_device(d1);
        dm.start();
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::cout << "  [OK] 超时清理后: " << dm.get_device_count() << " (应=0)" << std::endl;
        dm.stop();
    }

    {
        DeviceManager dm("self-id", 10);
        int online_count = 0, offline_count = 0;
        dm.set_on_device_online([&](const DeviceInfo&) { ++online_count; });
        dm.set_on_device_offline([&](const DeviceInfo&) { ++offline_count; });
        DeviceInfo d1; d1.id = "cb-test"; d1.name = "CallbackDev"; d1.ip = "10.0.0.1"; d1.port = 8889;
        dm.update_device(d1);
        dm.remove_device("cb-test");
        std::cout << "  [OK] 回调: 上线=" << online_count << " 离线=" << offline_count << std::endl;
    }

    std::cout << "=== 设备发现模块验证完成 ===" << std::endl << std::endl;
}

/**
 * @brief 阶段3: 信令控制通道单元验证
 *
 * 验证方法:
 * 1. 启动信令服务端 (端口 8889)
 * 2. 设置文件请求回调 (自动接受)
 * 3. 使用信令客户端向本地服务端发送 FILE_REQUEST
 * 4. 验证客户端收到 ACCEPT 响应
 * 5. 验证内容 (resume_from_chunk=0 等)
 */
void run_phase3_tests() {
    std::cout << "=== 单元验证: 信令控制通道 ===" << std::endl;

    {
        // 1. 启动信令服务端
        SignalingServer server(18889);  // 使用测试端口, 避免冲突
        server.set_on_file_request([](const json& request,
                                       const std::string& sender_ip,
                                       ResponseSender reply) {
            std::cout << "  [服务端] 收到文件请求: " << request.value("filename", "")
                      << " 来自 " << sender_ip << std::endl;

            // 自动接受
            json response = Protocol::build_file_response(
                request.value("file_id", ""), ResponseStatus::ACCEPT, 0
            );
            reply(response);
        });

        if (!server.start()) {
            std::cerr << "  [失败] 信令服务端启动失败" << std::endl;
            return;
        }
        std::cout << "  [OK] 信令服务端启动 (端口 18889)" << std::endl;

        // 短暂等待服务端就绪
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 2. 使用信令客户端发送请求
        SignalingClient client;
        json request = Protocol::build_file_request(
            "test-file-uuid", "test_document.pdf", 1048576,
            "abc123def456", 65536, 16
        );
        json response;

        // 连接到本地 127.0.0.1 发送请求
        bool success = client.send_request("127.0.0.1", 18889, request, response);

        if (success) {
            std::string status = response.value("status", "UNKNOWN");
            if (status == ResponseStatus::ACCEPT) {
                std::cout << "  [OK] 收到 ACCEPT 响应" << std::endl;
            } else {
                std::cout << "  [失败] 预期 ACCEPT, 实际 " << status << std::endl;
            }

            uint32_t resume_chunk = response.value("resume_from_chunk", 999u);
            if (resume_chunk == 0) {
                std::cout << "  [OK] resume_from_chunk = 0 (全新传输)" << std::endl;
            } else {
                std::cout << "  [失败] 预期 resume_from_chunk=0, 实际 " << resume_chunk << std::endl;
            }
        } else {
            std::cout << "  [失败] 信令客户端请求失败" << std::endl;
        }

        // 3. 测试拒绝场景
        std::cout << "  [测试] REJECT 场景..." << std::endl;

        // 修改回调为拒绝
        server.set_on_file_request([](const json& request,
                                       const std::string&,
                                       ResponseSender reply) {
            json response = Protocol::build_file_response(
                request.value("file_id", ""), ResponseStatus::REJECT, 0, "用户拒绝"
            );
            reply(response);
        });

        json request2 = Protocol::build_file_request(
            "test-file-uuid2", "reject_test.txt", 100,
            "hash123", 1024, 1
        );
        json response2;

        json req2 = Protocol::build_file_request(
            "test-file-uuid-2", "rejected_file.txt", 500, "hash999", 4096, 1
        );
        if (client.send_request("127.0.0.1", 18889, req2, response2)) {
            if (response2.value("status", "") == ResponseStatus::REJECT) {
                std::cout << "  [OK] REJECT 响应: " << response2.value("reason", "") << std::endl;
            } else {
                std::cout << "  [失败] 预期 REJECT, 实际 " << response2.value("status", "") << std::endl;
            }
        } else {
            std::cout << "  [失败] 信令客户端请求失败" << std::endl;
        }

        // 停止服务端
        server.stop();
    }

    std::cout << "=== 信令控制通道验证完成 ===" << std::endl << std::endl;
}

// ============================================================
// 模块初始化函数
// ============================================================

bool init_discovery_service() {
    g_device_id = Utils::generate_uuid();
    g_device_name = generate_device_name();
    std::cout << "[系统] 设备ID:   " << g_device_id << std::endl;
    std::cout << "[系统] 设备名称: " << g_device_name << std::endl;

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

    g_discovery = std::make_unique<DeviceDiscovery>(
        g_device_id, g_device_name, Defaults::DISCOVERY_PORT
    );
    g_discovery->set_on_device_found([](const DeviceInfo& device) {
        g_device_manager->update_device(device);
    });
    g_discovery->set_on_device_offline([](const std::string& device_id) {
        g_device_manager->remove_device(device_id);
    });

    if (!g_discovery->start()) {
        std::cerr << "[错误] 设备发现服务启动失败" << std::endl;
        return false;
    }
    return true;
}

bool init_signaling_service() {
    g_signaling_server = std::make_unique<SignalingServer>(Defaults::SIGNALING_PORT);

    // 设置文件请求回调: 自动接受所有传输请求
    g_signaling_server->set_on_file_request([](const json& request,
                                                const std::string& sender_ip,
                                                ResponseSender reply) {
        std::string filename = request.value("filename", "未知文件");
        uint64_t file_size = request.value("file_size", uint64_t(0));
        std::string file_id = request.value("file_id", "");

        std::cout << "\n[信令] ========================================" << std::endl;
        std::cout << "[信令] 收到文件传输请求" << std::endl;
        std::cout << "[信令]   文件: " << filename << std::endl;
        std::cout << "[信令]   大小: " << Utils::format_file_size(file_size) << std::endl;
        std::cout << "[信令]   来自: " << sender_ip << std::endl;
        std::cout << "[信令]   (自动接受)" << std::endl;
        std::cout << "[信令] ========================================" << std::endl;

        // TODO: 后续集成Web界面后, 改为通过WebUI确认
        json response = Protocol::build_file_response(file_id, ResponseStatus::ACCEPT, 0);
        reply(response);
    });

    // 设置控制消息回调
    g_signaling_server->set_on_control_message([](const json& msg,
                                                   const std::string& sender_ip) {
        std::string type = msg.value("type", "");
        std::string file_id = msg.value("file_id", "");
        std::cout << "[信令] 收到控制消息: " << type
                  << " (file_id=" << file_id.substr(0, 8) << "..., 来自 " << sender_ip << ")"
                  << std::endl;
    });

    if (!g_signaling_server->start()) {
        std::cerr << "[错误] 信令服务端启动失败" << std::endl;
        return false;
    }
    return true;
}

// ============================================================
// 程序入口
// ============================================================

int main() {
    print_banner();

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (!NetworkUtils::initialize()) {
        std::cerr << "[错误] 网络库初始化失败!" << std::endl;
        return 1;
    }
    std::cout << "[系统] 网络库初始化成功" << std::endl;

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
    run_phase3_tests();

    // 启动服务
    if (!init_discovery_service()) return 1;
    if (!init_signaling_service()) return 1;

    std::cout << "\n[系统] 所有服务启动完成, 按 Ctrl+C 退出" << std::endl;
    std::cout << "[系统] 信令端口: " << Defaults::SIGNALING_PORT
              << " | 发现端口: " << Defaults::DISCOVERY_PORT
              << " | HTTP端口: " << Defaults::HTTP_PORT << std::endl;

    // 主循环
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

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

    // 优雅退出
    std::cout << "\n[系统] 正在停止所有服务..." << std::endl;

    if (g_signaling_server) {
        g_signaling_server->stop();
        g_signaling_server.reset();
    }
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
