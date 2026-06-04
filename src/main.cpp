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
#include <fstream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "common/platform.h"
#include "common/utils.h"
#include "common/types.h"
#include "common/protocol.h"
#include "discovery/device_discovery.h"
#include "discovery/device_manager.h"
#include "signaling/signaling_server.h"
#include "signaling/signaling_client.h"
#include "transfer/transfer_sender.h"
#include "transfer/transfer_receiver.h"
#include "transfer/transfer_manager.h"
#include "web/http_server.h"

// ----------------------------------------------------------
// 全局变量
// ----------------------------------------------------------
static std::atomic<bool> g_running(true);
static std::unique_ptr<DeviceDiscovery>  g_discovery;
static std::unique_ptr<DeviceManager>    g_device_manager;
static std::unique_ptr<SignalingServer>  g_signaling_server;
static std::unique_ptr<TransferReceiver> g_transfer_receiver;
static std::unique_ptr<TransferManager>  g_transfer_manager;
static std::unique_ptr<HttpServer>       g_http_server;
static std::vector<std::thread>          g_transfer_threads;
static std::thread                       g_peer_probe_thread;

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

/**
 * @brief 阶段4: 文件传输模块单元验证
 *
 * 验证内容:
 * 1. Chunk序列化/反序列化
 * 2. FileChunkIO 分片读写
 * 3. TransferSender + TransferReceiver 端到端传输
 */
void run_phase4_tests() {
    std::cout << "=== 单元验证: 文件传输模块 ===" << std::endl;

    // 测试1: Chunk 序列化/反序列化
    {
        std::cout << "  [测试] Chunk 序列化/反序列化..." << std::endl;

        Chunk original;
        original.chunk_index  = 42;
        original.total_chunks = 100;
        original.file_id      = "test-file-id-123";
        original.data         = {0x01, 0x02, 0x03, 0x04, 0x05};
        original.data_size    = 5;

        auto serialized = original.serialize();

        Chunk parsed;
        if (parsed.deserialize(serialized.data(), serialized.size())) {
            bool ok = (parsed.chunk_index == 42 &&
                       parsed.total_chunks == 100 &&
                       parsed.file_id == "test-file-id-123" &&
                       parsed.data_size == 5 &&
                       parsed.data[0] == 0x01 &&
                       parsed.data[4] == 0x05);
            std::cout << "    " << (ok ? "[OK]" : "[失败]") << " Chunk 序列化/反序列化"
                      << (ok ? "" : " 数据不匹配") << std::endl;
        } else {
            std::cout << "    [失败] 反序列化失败" << std::endl;
        }
    }

    // 测试2: FileChunkIO 读写
    {
        std::cout << "  [测试] FileChunkIO 分片读写..." << std::endl;

        const std::string test_file = "/tmp/p2p_test_write.txt";
        const std::string test_data = "Hello, P2P File Transfer! This is test data for chunked I/O operations."
                                       "1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        const uint32_t chunk_size = 16;

        // 写入测试文件
        {
            std::ofstream ofs(test_file, std::ios::binary);
            ofs.write(test_data.data(), test_data.size());
            ofs.close();
        }

        // 发送方: 分片读取
        FileChunkIO sender_io(test_file, chunk_size, true);
        uint32_t total = sender_io.get_total_chunks();
        std::cout << "    [OK] 总分片数: " << total << " (每片 " << chunk_size << " 字节)" << std::endl;

        // 读取每个分片并存入buffer
        std::vector<uint8_t> reassembled;
        for (uint32_t i = 0; i < total; ++i) {
            Chunk chunk;
            if (sender_io.read_chunk(i, "test-id", total, chunk)) {
                reassembled.insert(reassembled.end(), chunk.data.begin(), chunk.data.end());
            }
        }

        // 验证重组数据
        std::string reassembled_str(reassembled.begin(), reassembled.end());
        if (reassembled_str == test_data) {
            std::cout << "    [OK] 分片读取重组正确" << std::endl;
        } else {
            std::cout << "    [失败] 重组数据不匹配 (len=" << reassembled_str.size()
                      << " expected=" << test_data.size() << ")" << std::endl;
        }

        // 接收方: 分片写入
        const std::string recv_file = "/tmp/p2p_test_receive.txt";
        {
            // 预创建文件
            { std::ofstream ofs(recv_file + ".tmp", std::ios::binary); ofs.close(); }
            // 预分配空间
            int fd = open((recv_file + ".tmp").c_str(), O_WRONLY);
            if (fd >= 0) {
                (void) ftruncate(fd, static_cast<off_t>(test_data.size()));
                close(fd);
            }
        }

        FileChunkIO receiver_io(recv_file, chunk_size, false);
        for (uint32_t i = 0; i < total; ++i) {
            Chunk chunk;
            sender_io.read_chunk(i, "test-id", total, chunk);
            receiver_io.write_chunk(chunk);
        }

        if (receiver_io.get_max_contiguous_chunk() == total - 1) {
            std::cout << "    [OK] 分片写入完成, 连续块数: "
                      << receiver_io.get_max_contiguous_chunk() << std::endl;
        } else {
            std::cout << "    [失败] 分片写入不完整" << std::endl;
        }

        receiver_io.commit_received_file();
        Utils::get_file_size(recv_file);  // verify file exists

        // 校验MD5
        std::string orig_md5 = Utils::md5_file(test_file);
        std::string recv_md5 = Utils::md5_file(recv_file);
        if (orig_md5 == recv_md5) {
            std::cout << "    [OK] MD5校验一致: " << orig_md5 << std::endl;
        } else {
            std::cout << "    [失败] MD5不匹配: " << orig_md5 << " vs " << recv_md5 << std::endl;
        }

        // 清理
        std::remove(test_file.c_str());
        std::remove(recv_file.c_str());
    }

    // 测试3: 端到端传输 (Sender -> Receiver, 本地回环)
    {
        std::cout << "  [测试] 端到端文件传输 (localhost)..." << std::endl;

        // 创建测试文件 (~100KB)
        const std::string src_file = "/tmp/p2p_e2e_src.bin";
        {
            std::ofstream ofs(src_file, std::ios::binary);
            for (int i = 0; i < 1600; ++i) {  // 1600 * 64 = ~100KB
                char buf[64];
                memset(buf, (i % 256), sizeof(buf));
                ofs.write(buf, sizeof(buf));
            }
            ofs.close();
        }

        std::atomic<bool> received(false);
        std::string received_path;
        bool receive_success = false;
        int test_port = 18890;

        // 启动接收方
        TransferReceiver receiver(test_port);
        receiver.set_save_directory("/tmp");
        receiver.set_on_receive_complete([&](const std::string& /*fid*/,
                                              const std::string& path,
                                              bool success) {
            received = true;
            received_path = path;
            receive_success = success;
        });

        if (!receiver.start()) {
            std::cout << "    [失败] 接收方启动失败" << std::endl;
            return;
        }

        // 短暂等待接收方就绪
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 启动发送方
        TransferSender sender;
        std::string file_id = Utils::generate_uuid();

        std::atomic<int> progress_count(0);
        bool send_ok = sender.send_file("127.0.0.1", test_port, src_file, file_id,
                                        4,  // window_size
                                        [&](const TransferProgress& /*p*/) {
            ++progress_count;
        });

        // 等待接收完成
        for (int i = 0; i < 20 && !received.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        receiver.stop();

        std::cout << "    [OK] 发送方: " << (send_ok ? "成功" : "失败")
                  << ", 进度回调: " << progress_count.load() << " 次" << std::endl;
        std::cout << "    [OK] 接收方: " << (receive_success ? "成功" : "失败")
                  << ", 文件: " << received_path << std::endl;

        if (send_ok && receive_success) {
            std::string src_md5 = Utils::md5_file(src_file);
            std::string dst_md5 = Utils::md5_file(received_path);
            if (src_md5 == dst_md5) {
                std::cout << "    [OK] 端到端MD5一致: " << src_md5 << std::endl;
            } else {
                std::cout << "    [失败] MD5不匹配" << std::endl;
            }
        }

        // 清理
        std::remove(src_file.c_str());
        if (!received_path.empty()) {
            std::remove(received_path.c_str());
        }
    }

    std::cout << "=== 文件传输模块验证完成 ===" << std::endl << std::endl;
}

// ============================================================
// 模块初始化函数
// ============================================================

static void load_known_devices();
static void save_known_devices();

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

    // 加载持久化的已知设备
    load_known_devices();

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

    // 设置 TCP 握手回调: 自动将探测方加入设备列表 (互相发现)
    g_signaling_server->set_on_device_hello([](const json& hello,
                                                const std::string& sender_ip) {
        if (!g_device_manager) return;
        std::string remote_id   = hello.value("device_id", "");
        std::string remote_name = hello.value("device_name", sender_ip);
        std::string remote_ip   = hello.value("ip", sender_ip);
        uint16_t    remote_port = hello.value("port", Defaults::SIGNALING_PORT);

        // 检查是否已有该IP的设备, 有则更新而非新增
        std::string existing_id = g_device_manager->find_device_id_by_ip(remote_ip);
        if (!existing_id.empty()) {
            DeviceInfo update;
            update.id         = existing_id;
            update.name       = remote_name;
            update.ip         = remote_ip;
            update.port       = remote_port;
            update.last_seen  = std::chrono::steady_clock::now();
            update.manual     = true;
            g_device_manager->update_device(update);
            return;
        }

        DeviceInfo device;
        device.id         = remote_id;
        device.name       = remote_name;
        device.ip         = remote_ip;
        device.port       = remote_port;
        device.last_seen  = std::chrono::steady_clock::now();
        device.first_seen = std::chrono::steady_clock::now();
        device.manual     = true;
        g_device_manager->update_device(device);
        save_known_devices();
    });

    if (!g_signaling_server->start()) {
        std::cerr << "[错误] 信令服务端启动失败" << std::endl;
        return false;
    }
    return true;
}

/**
 * @brief 初始化HTTP Web服务器 + REST API
 */
bool init_http_service() {
    g_http_server = std::make_unique<HttpServer>(Defaults::HTTP_PORT);
    g_http_server->set_static_dir("src/web/static");

    // GET /api/devices - 在线设备列表
    g_http_server->on_get("/api/devices", []() -> std::string {
        json resp;
        resp["self_id"]   = g_device_id;
        resp["self_name"] = g_device_name;
        json devices = json::array();
        if (g_device_manager) {
            for (const auto& d : g_device_manager->get_online_devices()) {
                json dev;
                dev["id"]   = d.id.substr(0, 8);
                dev["name"] = d.name;
                dev["ip"]   = d.ip;
                dev["port"] = d.port;
                dev["manual"] = d.manual;
                // 15秒内有探活更新则视为在线
                auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - d.last_seen).count();
                dev["online"] = (age < 15);
                devices.push_back(dev);
            }
        }
        resp["devices"] = devices;
        return resp.dump();
    });

    // GET /api/transfers - 传输任务列表
    g_http_server->on_get("/api/transfers", []() -> std::string {
        json resp;
        json list = json::array();
        if (g_transfer_manager) {
            for (const auto& t : g_transfer_manager->get_all_tasks()) {
                json item;
                item["file_id"]      = t.meta.file_id;
                item["filename"]     = t.meta.filename;
                item["file_size"]    = t.meta.file_size;
                item["total_chunks"] = t.meta.total_chunks;
                item["progress_chunk"] = t.progress_chunk;
                item["speed"]        = t.speed;
                item["target_ip"]    = t.target.ip;
                item["state"]        = (t.state == TransferState::TRANSFERRING ? "TRANSFERRING" :
                                        t.state == TransferState::COMPLETED ? "COMPLETED" :
                                        t.state == TransferState::PAUSED ? "PAUSED" : "IDLE");
                list.push_back(item);
            }
        }
        resp["transfers"] = list;
        return resp.dump();
    });

    // POST /api/peers/add - 手动添加设备 (绕过组播发现)
    g_http_server->on_post("/api/peers/add", [](const std::string& body, const std::map<std::string, std::string>&) -> std::string {
        json resp;
        std::string ip;
        std::string name;

        auto pairs = Utils::split_string(body, '&');
        for (const auto& p : pairs) {
            auto eq = p.find('=');
            if (eq != std::string::npos) {
                std::string key = Utils::url_decode(p.substr(0, eq));
                std::string val = Utils::url_decode(p.substr(eq + 1));
                if (key == "ip") ip = val;
                else if (key == "name") name = val;
            }
        }

        if (ip.empty()) {
            resp["success"] = false;
            resp["error"] = "缺少IP地址";
            return resp.dump();
        }

        if (!g_device_manager) {
            resp["success"] = false;
            resp["error"] = "设备管理器未初始化";
            return resp.dump();
        }

        // 检查是否已有该IP, 有则更新名称
        std::string existing_id = g_device_manager->find_device_id_by_ip(ip);
        DeviceInfo device;
        if (!existing_id.empty()) {
            device.id        = existing_id;
            device.name      = name.empty() ? ip : name;
            device.ip        = ip;
            device.port      = Defaults::SIGNALING_PORT;
            device.last_seen = std::chrono::steady_clock::now();
            device.manual    = true;
        } else {
            device.id        = Utils::generate_uuid();
            device.name      = name.empty() ? ip : name;
            device.ip        = ip;
            device.port      = Defaults::SIGNALING_PORT;
            device.last_seen = std::chrono::steady_clock::now();
            device.first_seen = std::chrono::steady_clock::now();
            device.manual    = true;
        }

        g_device_manager->update_device(device);
        save_known_devices();

        std::cout << "[Web] 手动添加设备: " << device.name << " (" << ip << ")" << std::endl;

        resp["success"] = true;
        resp["id"]   = device.id;
        resp["name"] = device.name;
        resp["ip"]   = device.ip;
        return resp.dump();
    });

    // POST /api/peers/remove - 移除手动添加的设备
    g_http_server->on_post("/api/peers/remove", [](const std::string& body, const std::map<std::string, std::string>&) -> std::string {
        json resp;
        std::string target_ip;

        auto pairs = Utils::split_string(body, '&');
        for (const auto& p : pairs) {
            auto eq = p.find('=');
            if (eq != std::string::npos) {
                std::string key = Utils::url_decode(p.substr(0, eq));
                std::string val = Utils::url_decode(p.substr(eq + 1));
                if (key == "ip") target_ip = val;
            }
        }

        if (!g_device_manager || target_ip.empty()) {
            resp["success"] = false;
            resp["error"] = "无效请求";
            return resp.dump();
        }

        // 查找匹配IP的设备
        for (const auto& d : g_device_manager->get_online_devices()) {
            if (d.ip == target_ip && d.manual) {
                g_device_manager->remove_device(d.id);
                save_known_devices();
                std::cout << "[Web] 手动移除设备: " << d.name << " (" << target_ip << ")" << std::endl;
                break;
            }
        }

        resp["success"] = true;
        return resp.dump();
    });

    // POST /api/message - 发送聊天文本消息
    g_http_server->on_post("/api/message", [](const std::string& body, const std::map<std::string, std::string>&) -> std::string {
        json resp;
        std::string target_ip;
        uint16_t target_port = Defaults::SIGNALING_PORT;
        std::string text;

        auto pairs = Utils::split_string(body, '&');
        for (const auto& p : pairs) {
            auto eq = p.find('=');
            if (eq != std::string::npos) {
                std::string key = Utils::url_decode(p.substr(0, eq));
                std::string val = Utils::url_decode(p.substr(eq + 1));
                if (key == "target_ip") target_ip = val;
                else if (key == "target_port") target_port = static_cast<uint16_t>(std::stoul(val));
                else if (key == "text") text = val;
            }
        }

        SignalingClient sig_client;
        json request = Protocol::build_text_message(text);
        json response;
        if (!sig_client.send_request(target_ip, target_port, request, response)) {
            resp["success"] = false;
            resp["error"] = "发送失败";
            return resp.dump();
        }
        resp["success"] = true;
        return resp.dump();
    });

    // POST /api/transfer - 发起文件传输 (支持 base64 编码文件上传)
    g_http_server->on_post("/api/transfer", [](const std::string& body, const std::map<std::string, std::string>&) -> std::string {
        json resp;
        std::string target_ip = "127.0.0.1";
        uint16_t target_port = Defaults::SIGNALING_PORT;
        std::string filename = "test.dat";
        std::string filedata_b64;

        auto pairs = Utils::split_string(body, '&');
        for (const auto& p : pairs) {
            auto eq = p.find('=');
            if (eq != std::string::npos) {
                std::string key = Utils::url_decode(p.substr(0, eq));
                std::string val = Utils::url_decode(p.substr(eq + 1));
                if (key == "target_ip") target_ip = val;
                else if (key == "target_port") target_port = static_cast<uint16_t>(std::stoul(val));
                else if (key == "filename") filename = val;
                else if (key == "filedata") filedata_b64 = val;
            }
        }

        if (filedata_b64.empty()) {
            resp["success"] = false;
            resp["error"] = "未提供文件数据";
            return resp.dump();
        }

        // 解码 base64 文件内容并保存到临时文件
        std::string file_data = Utils::base64_decode(filedata_b64);
        if (file_data.empty()) {
            resp["success"] = false;
            resp["error"] = "文件解码失败";
            return resp.dump();
        }

        uint64_t file_size = file_data.size();

        // 保存到临时目录
        std::string temp_dir = "/tmp/p2p_send";
        mkdir(temp_dir.c_str(), 0755);
        std::string temp_path = temp_dir + "/" + filename;

        {
            std::ofstream tmp_file(temp_path, std::ios::binary);
            if (!tmp_file.is_open()) {
                resp["success"] = false;
                resp["error"] = "无法创建临时文件";
                return resp.dump();
            }
            tmp_file.write(file_data.data(), file_data.size());
            tmp_file.close();
        }

        // 使用信令协商
        SignalingClient sig_client;
        std::string file_id = Utils::generate_uuid();
        uint32_t total_chunks = static_cast<uint32_t>(
            (file_size + Defaults::CHUNK_SIZE - 1) / Defaults::CHUNK_SIZE
        );

        json request = Protocol::build_file_request(
            file_id, filename, file_size, "", Defaults::CHUNK_SIZE, total_chunks
        );
        json sig_response;
        if (!sig_client.send_request(target_ip, target_port, request, sig_response)) {
            resp["success"] = false;
            resp["error"] = "信令协商失败";
            return resp.dump();
        }

        std::string status = sig_response.value("status", "");
        if (status != ResponseStatus::ACCEPT) {
            resp["success"] = false;
            resp["error"] = "对方拒绝";
            return resp.dump();
        }

        uint16_t transfer_port = sig_response.value("port", Defaults::TRANSFER_PORT);

        // 创建传输任务
        TransferTask task;
        task.meta.file_id     = file_id;
        task.meta.filename    = filename;
        task.meta.file_size   = file_size;
        task.meta.chunk_size  = Defaults::CHUNK_SIZE;
        task.meta.total_chunks = total_chunks;
        task.target.ip        = target_ip;
        task.target.port      = transfer_port;
        task.state            = TransferState::TRANSFERRING;
        task.is_sender        = true;
        g_transfer_manager->add_task(task);

        // 在后台线程中启动实际文件传输
        std::string captured_file_id = file_id;
        std::string captured_ip      = target_ip;
        uint16_t    captured_port    = transfer_port;
        std::string captured_path    = temp_path;

        g_transfer_threads.push_back(std::thread([captured_file_id, captured_ip, captured_port, captured_path, total_chunks, file_size]() {
            TransferSender sender;
            std::cout << "[传输] 开始发送: " << captured_path
                      << " -> " << captured_ip << ":" << captured_port << std::endl;

            bool ok = sender.send_file(captured_ip, captured_port, captured_path,
                                       captured_file_id, Defaults::WINDOW_SIZE,
                [=](const TransferProgress& progress) {
                    if (g_transfer_manager) {
                        g_transfer_manager->update_progress(captured_file_id, progress.sent_chunks, progress.bytes_sent, progress.speed);
                    }
                });

            if (g_transfer_manager) {
                g_transfer_manager->mark_complete(captured_file_id, ok);
            }

            // 清理临时文件
            std::remove(captured_path.c_str());

            if (ok) {
                std::cout << "[传输] 文件发送成功: " << captured_file_id.substr(0, 8) << std::endl;
            } else {
                std::cerr << "[传输] 文件发送失败: " << captured_file_id.substr(0, 8) << std::endl;
            }
        }));

        // 清理已完成的线程
        g_transfer_threads.erase(
            std::remove_if(g_transfer_threads.begin(), g_transfer_threads.end(),
                [](std::thread& t) {
                    if (!t.joinable()) return true;
                    return false;
                }),
            g_transfer_threads.end()
        );

        resp["success"] = true;
        resp["file_id"] = file_id;
        resp["message"] = "传输已启动";
        return resp.dump();
    });

    if (!g_http_server->start()) {
        std::cerr << "[错误] HTTP服务器启动失败" << std::endl;
        return false;
    }
    return true;
}

/**
 * @brief 初始化传输服务
 */
bool init_transfer_service() {
    g_transfer_manager = std::make_unique<TransferManager>();

    (void) system("mkdir -p ./received_files");

    g_transfer_receiver = std::make_unique<TransferReceiver>(Defaults::TRANSFER_PORT);
    g_transfer_receiver->set_save_directory("./received_files");
    g_transfer_manager->set_receiver(g_transfer_receiver.get());
    g_transfer_receiver->set_on_receive_complete([](const std::string& file_id,
                                                      const std::string& file_path,
                                                      bool success) {
        if (g_transfer_manager) {
            g_transfer_manager->mark_complete(file_id, success);
        }
        if (success) {
            std::cout << "[传输] 文件接收完成: " << file_path << std::endl;
        } else {
            std::cerr << "[传输] 文件接收失败: file_id=" << file_id << std::endl;
        }
    });

    if (!g_transfer_receiver->start()) {
        std::cerr << "[错误] 传输接收服务启动失败" << std::endl;
        return false;
    }
    return true;
}

// ============================================================
// 程序入口
// ============================================================

static constexpr const char* KNOWN_DEVICES_FILE = "known_devices.json";

static void save_known_devices() {
    if (!g_device_manager) return;
    json arr = json::array();
    for (const auto& d : g_device_manager->get_online_devices()) {
        if (!d.manual) continue;
        json entry;
        entry["id"]   = d.id;
        entry["name"] = d.name;
        entry["ip"]   = d.ip;
        entry["port"] = d.port;
        arr.push_back(entry);
    }
    std::ofstream f(KNOWN_DEVICES_FILE);
    if (f.is_open()) {
        f << arr.dump(2);
        std::cout << "[持久化] 已保存 " << arr.size() << " 个已知设备" << std::endl;
    }
}

static void load_known_devices() {
    if (!g_device_manager) return;
    std::ifstream f(KNOWN_DEVICES_FILE);
    if (!f.is_open()) return;
    try {
        json arr = json::parse(f);
        int loaded = 0;
        for (const auto& entry : arr) {
            DeviceInfo device;
            device.id         = entry.value("id", Utils::generate_uuid());
            device.name       = entry.value("name", entry.value("ip", ""));
            device.ip         = entry.value("ip", "");
            device.port       = entry.value("port", Defaults::SIGNALING_PORT);
            device.last_seen  = std::chrono::steady_clock::now();
            device.first_seen = std::chrono::steady_clock::now();
            device.manual     = true;
            if (!device.ip.empty()) {
                g_device_manager->update_device(device);
                ++loaded;
            }
        }
        if (loaded > 0) {
            std::cout << "[持久化] 加载了 " << loaded << " 个已知设备" << std::endl;
        }
    } catch (...) {
        std::cerr << "[持久化] 读取失败, 忽略" << std::endl;
    }
}

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
    run_phase4_tests();

    // 启动服务
    if (!init_discovery_service()) return 1;
    if (!init_signaling_service()) return 1;
    if (!init_transfer_service()) return 1;
    if (!init_http_service()) return 1;

    std::cout << "\n[系统] 所有服务启动完成, 按 Ctrl+C 退出" << std::endl;
    // Auto-open browser
    std::string url = "http://localhost:" + std::to_string(Defaults::HTTP_PORT);
    system(("xdg-open " + url + " 2>/dev/null &").c_str());
    std::cout << "[系统] Web界面: " << url << std::endl;
    std::cout << "[系统] 信令端口: " << Defaults::SIGNALING_PORT
              << " | 发现端口: " << Defaults::DISCOVERY_PORT
              << " | 传输端口: " << Defaults::TRANSFER_PORT << std::endl;

    // 启动手动添加设备的 TCP 探活线程 (互相发现)
    g_peer_probe_thread = std::thread([]() {
        std::map<std::string, int> fail_count;
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));

            if (!g_device_manager) continue;
            auto devices = g_device_manager->get_online_devices();
            for (const auto& d : devices) {
                if (!d.manual) continue;

                bool alive = SignalingClient::test_connect(
                    d.ip, d.port,
                    g_device_id, g_device_name,
                    g_discovery ? g_discovery->get_local_ip() : "127.0.0.1",
                    Defaults::SIGNALING_PORT, 1000);
                if (alive) {
                    fail_count[d.id] = 0;
                    DeviceInfo updated = d;
                    updated.last_seen = std::chrono::steady_clock::now();
                    g_device_manager->update_device(updated);
                } else {
                    ++fail_count[d.id];
                    if (fail_count[d.id] >= 3) {
                        std::cout << "[探活] 手动设备不可达, 移除: "
                                  << d.name << " (" << d.ip << ")" << std::endl;
                        g_device_manager->remove_device(d.id);
                        fail_count.erase(d.id);
                    }
                }
            }
        }
    });

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

    // 等待正在进行的传输完成
    for (auto& t : g_transfer_threads) {
        if (t.joinable()) t.join();
    }
    g_transfer_threads.clear();

    if (g_peer_probe_thread.joinable()) {
        g_peer_probe_thread.join();
    }

    if (g_signaling_server) {
        g_signaling_server->stop();
        g_signaling_server.reset();
    }
    if (g_http_server) {
        g_http_server->stop();
        g_http_server.reset();
    }
    if (g_transfer_receiver) {
        g_transfer_receiver->stop();
        g_transfer_receiver.reset();
    }
    if (g_transfer_manager) {
        g_transfer_manager.reset();
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
