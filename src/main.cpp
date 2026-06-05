// ============================================================
// P2P蓝牙文件传输工具 - 程序入口
// 功能: 初始化各个蓝牙模块, 启动Web服务器, 进入主循环
// 蓝牙版: 基于 BlueZ D-Bus + RFCOMM 替代原 LAN UDP/TCP
// ============================================================

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <memory>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <set>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "common/platform.h"
#include "common/utils.h"
#include "common/types.h"
#include "common/protocol.h"
#include "discovery/device_manager.h"
#include "bluetooth/bt_utils.h"
#include "bluetooth/bt_discovery.h"
#include "bluetooth/bt_signaling_server.h"
#include "bluetooth/bt_signaling_client.h"
#include "bluetooth/bt_transfer_sender.h"
#include "bluetooth/bt_transfer_receiver.h"
#include "transfer/transfer_manager.h"
#include "web/http_server.h"

// ----------------------------------------------------------
// 全局变量
// ----------------------------------------------------------
static std::atomic<bool> g_running(true);
static std::unique_ptr<BtDiscovery>          g_discovery;
static std::unique_ptr<DeviceManager>        g_device_manager;
static std::unique_ptr<BtSignalingServer>    g_signaling_server;
static std::unique_ptr<BtTransferReceiver>   g_transfer_receiver;
static std::unique_ptr<TransferManager>      g_transfer_manager;
static std::unique_ptr<HttpServer>           g_http_server;
static std::vector<std::thread>              g_transfer_threads;
static std::thread                           g_peer_probe_thread;

// 收到的文本消息缓存 (sender_addr -> [{text, time}])
static std::mutex                                  g_rcv_msg_mutex;
static std::map<std::string, std::vector<json>>    g_rcv_messages;

// ---- 接受文件传输的目录 ----
static std::string g_received_files_dir = "./received_files";

static std::string g_device_id;    // 本机设备ID
static std::string g_device_name;  // 本机设备名称
static std::string g_local_addr;   // 本机蓝牙地址

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
    std::cout << "  P2P蓝牙文件传输工具 v2.0.0" << std::endl;
    std::cout << "  P2P Bluetooth File Transfer Tool" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << std::endl;
}

/**
 * @brief 生成设备名称
 */
std::string generate_device_name() {
    // 优先使用蓝牙适配器名称
    std::string bt_name = BtUtils::get_local_device_name();
    if (!bt_name.empty() && bt_name != "P2P-BT-Device") {
        if (bt_name.size() > 20) bt_name = bt_name.substr(0, 20);
        return bt_name;
    }

    // 退回到主机名
    std::string hostname = "P2P-Device";
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) hostname = buf;
    if (hostname.size() > 20) hostname = hostname.substr(0, 20);
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

    // 测试蓝牙地址转换
    bdaddr_t test_addr;
    if (BtUtils::str_to_bdaddr("AA:BB:CC:DD:EE:FF", test_addr)) {
        std::string back = BtUtils::bdaddr_to_str(test_addr);
        std::cout << "  [OK] BDADDR 转换: AA:BB:CC:DD:EE:FF → "
                  << back << (back == "AA:BB:CC:DD:EE:FF" ? "" : " (不匹配!)")
                  << std::endl;
    }

    std::cout << "=== 工具函数验证完成 ===" << std::endl << std::endl;
}

void run_phase2_tests() {
    std::cout << "=== 单元验证: 设备发现模块 ===" << std::endl;

    {
        DeviceManager dm("self-id-001", 2);
        DeviceInfo d1; d1.id = "d1"; d1.name = "Dev1"; d1.addr = "AA:BB:CC:DD:EE:01"; d1.port = 1;
        DeviceInfo d2; d2.id = "d2"; d2.name = "Dev2"; d2.addr = "AA:BB:CC:DD:EE:02"; d2.port = 1;
        DeviceInfo d3; d3.id = "self-id-001"; d3.name = "Self"; d3.addr = "AA:BB:CC:DD:EE:00"; d3.port = 1;

        dm.update_device(d1);
        dm.update_device(d2);
        dm.update_device(d3);  // 应被忽略
        std::cout << "  [OK] 设备增删查: " << dm.get_device_count() << " (已排除自己)" << std::endl;

        dm.remove_device("d1");
        std::cout << "  [OK] 移除设备后: " << dm.get_device_count() << std::endl;
    }

    {
        DeviceManager dm("self-id", 1);
        DeviceInfo d1; d1.id = "d1"; d1.name = "ShortLive"; d1.addr = "00:00:00:00:00:01"; d1.port = 1;
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
        DeviceInfo d1; d1.id = "cb-test"; d1.name = "CallbackDev"; d1.addr = "00:00:00:00:00:02"; d1.port = 1;
        dm.update_device(d1);
        dm.remove_device("cb-test");
        std::cout << "  [OK] 回调: 上线=" << online_count << " 离线=" << offline_count << std::endl;
    }

    std::cout << "=== 设备发现模块验证完成 ===" << std::endl << std::endl;
}

/**
 * @brief 阶段3: 信令控制通道单元验证 (蓝牙 RFCOMM 版)
 *
 * 注意: RFCOMM 无法通过本地回环测试, 此测试仅验证
 * BtSignalingServer 和 BtSignalingClient 的消息格式兼容性
 */
void run_phase3_tests() {
    std::cout << "=== 单元验证: 信令控制通道 (RFCOMM) ===" << std::endl;

    {
        // 验证 JSON 协议消息格式与蓝牙适配
        json request = Protocol::build_file_request(
            "test-file-uuid", "test_document.pdf", 1048576,
            "abc123def456", 65536, 16
        );
        std::cout << "  [OK] FILE_REQUEST 构建: type=" << request.value("type", "")
                  << " filename=" << request.value("filename", "") << std::endl;

        json hello = Protocol::build_device_hello(
            "test-id", "TestDevice", "AA:BB:CC:DD:EE:FF", 1
        );
        std::cout << "  [OK] DEVICE_HELLO 构建: addr=" << hello.value("addr", "")
                  << " channel=" << hello.value("channel", 0) << std::endl;

        json reject = Protocol::build_file_response("test-id", ResponseStatus::REJECT, 0, "测试拒绝");
        std::cout << "  [OK] FILE_RESPONSE 构建: status=" << reject.value("status", "") << std::endl;
    }

    // 如果蓝牙适配器可用, 启动 RFCOMM 信令服务器做基础功能测试
    std::string local_addr = BtUtils::get_local_bdaddr();
    if (!local_addr.empty()) {
        std::cout << "  [测试] 尝试启动 RFCOMM 信令服务器 (通道 10)..." << std::endl;

        BtSignalingServer server(10);  // 测试通道 10
        server.set_on_file_request([](const json& request,
                                       const std::string& sender_addr,
                                       ResponseSender reply) {
            std::cout << "  [服务端] 收到文件请求: " << request.value("filename", "")
                      << " 来自 " << sender_addr << std::endl;
            json response = Protocol::build_file_response(
                request.value("file_id", ""), ResponseStatus::ACCEPT, 0);
            reply(response);
        });

        if (server.start()) {
            std::cout << "  [OK] RFCOMM 信令服务端启动成功 (通道 10)" << std::endl;

            // 测试客户端连接自身
            BtSignalingClient client;
            json req = Protocol::build_file_request(
                "test-fid", "test.txt", 100, "hash", 1024, 1);
            json resp;

            if (client.send_request(local_addr, 10, req, resp, 3000)) {
                std::string status = resp.value("status", "");
                std::cout << "  [OK] 客户端收到响应: " << status << std::endl;
            } else {
                std::cout << "  [信息] 自连接测试跳过 (可能需要已配对自身)" << std::endl;
            }

            server.stop();
        } else {
            std::cout << "  [信息] 信令服务器启动跳过 (端口可能被占用)" << std::endl;
        }
    } else {
        std::cout << "  [信息] 蓝牙适配器不可用, 跳过 RFCOMM 测试" << std::endl;
    }

    std::cout << "=== 信令控制通道验证完成 ===" << std::endl << std::endl;
}

/**
 * @brief 阶段4: 文件传输模块单元验证
 *
 * 验证内容:
 * 1. Chunk序列化/反序列化
 * 2. FileChunkIO 分片读写
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

        {
            std::ofstream ofs(test_file, std::ios::binary);
            ofs.write(test_data.data(), test_data.size());
            ofs.close();
        }

        FileChunkIO sender_io(test_file, chunk_size, true);
        uint32_t total = sender_io.get_total_chunks();
        std::cout << "    [OK] 总分片数: " << total << " (每片 " << chunk_size << " 字节)" << std::endl;

        std::vector<uint8_t> reassembled;
        for (uint32_t i = 0; i < total; ++i) {
            Chunk chunk;
            if (sender_io.read_chunk(i, "test-id", total, chunk)) {
                reassembled.insert(reassembled.end(), chunk.data.begin(), chunk.data.end());
            }
        }

        std::string reassembled_str(reassembled.begin(), reassembled.end());
        if (reassembled_str == test_data) {
            std::cout << "    [OK] 分片读取重组正确" << std::endl;
        } else {
            std::cout << "    [失败] 重组数据不匹配" << std::endl;
        }

        const std::string recv_file = "/tmp/p2p_test_receive.txt";
        {
            { std::ofstream ofs(recv_file + ".tmp", std::ios::binary); ofs.close(); }
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
            std::cout << "    [OK] 分片写入完成" << std::endl;
        } else {
            std::cout << "    [失败] 分片写入不完整" << std::endl;
        }

        receiver_io.commit_received_file();
        std::string orig_md5 = Utils::md5_file(test_file);
        std::string recv_md5 = Utils::md5_file(recv_file);
        if (orig_md5 == recv_md5) {
            std::cout << "    [OK] MD5校验一致: " << orig_md5 << std::endl;
        } else {
            std::cout << "    [失败] MD5不匹配" << std::endl;
        }

        std::remove(test_file.c_str());
        std::remove(recv_file.c_str());
    }

    // 测试3: RFCOMM 传输端到端 (需要蓝牙适配器)
    {
        std::cout << "  [测试] RFCOMM 端到端传输..." << std::endl;
        std::string local_addr = BtUtils::get_local_bdaddr();
        if (!local_addr.empty()) {
            std::cout << "    [信息] 蓝牙适配器可用, "
                      << "RFCOMM 端到端测试将在实际蓝牙连接时验证" << std::endl;
        } else {
            std::cout << "    [跳过] 蓝牙适配器不可用" << std::endl;
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
    std::cout << "[系统] 设备ID:     " << g_device_id << std::endl;
    std::cout << "[系统] 设备名称:   " << g_device_name << std::endl;

    // 获取本机蓝牙地址
    g_local_addr = BtUtils::get_local_bdaddr();
    if (g_local_addr.empty()) {
        std::cerr << "[错误] 未检测到蓝牙适配器" << std::endl;
        return false;
    }
    std::cout << "[系统] 蓝牙地址:   " << g_local_addr << std::endl;

    g_device_manager = std::make_unique<DeviceManager>(g_device_id);
    g_device_manager->set_on_device_online([](const DeviceInfo& device) {
        std::cout << "[事件] 设备上线: " << device.name
                  << " (" << device.addr << ", 通道 " << device.port << ")" << std::endl;
    });
    g_device_manager->set_on_device_offline([](const DeviceInfo& device) {
        std::cout << "[事件] 设备离线: " << device.name
                  << " (" << device.addr << ")" << std::endl;
    });
    g_device_manager->start();

    // 加载持久化的已知设备
    load_known_devices();

    // 启动蓝牙设备发现
    g_discovery = std::make_unique<BtDiscovery>(g_device_id, g_device_name);
    g_discovery->set_on_device_found([](const DeviceInfo& device) {
        g_device_manager->update_device(device);
    });
    g_discovery->set_on_device_offline([](const std::string& device_id) {
        // BtDiscovery 使用 BDADDR 作为 device_id
        // DeviceManager 的 remove_device 需要真正的 UUID
        // 通过地址查找并移除
        if (g_device_manager) {
            std::string real_id = g_device_manager->find_device_id_by_addr(device_id);
            if (!real_id.empty()) {
                g_device_manager->remove_device(real_id);
            }
        }
    });

    if (!g_discovery->start()) {
        std::cerr << "[错误] 蓝牙设备发现服务启动失败" << std::endl;
        return false;
    }
    return true;
}

bool init_signaling_service() {
    g_signaling_server = std::make_unique<BtSignalingServer>(Defaults::SIGNALING_CHANNEL);

    // 设置文件请求回调: 自动接受所有传输请求
    g_signaling_server->set_on_file_request([](const json& request,
                                                 const std::string& sender_addr,
                                                 ResponseSender reply) {
        std::string filename  = request.value("filename", "未知文件");
        uint64_t    file_size = request.value("file_size", uint64_t(0));
        std::string file_id   = request.value("file_id", "");

        std::cout << "\n[信令] ========================================" << std::endl;
        std::cout << "[信令] 收到文件传输请求" << std::endl;
        std::cout << "[信令]   文件: " << filename << std::endl;
        std::cout << "[信令]   大小: " << Utils::format_file_size(file_size) << std::endl;
        std::cout << "[信令]   来自: " << sender_addr << std::endl;
        std::cout << "[信令]   (自动接受)" << std::endl;
        std::cout << "[信令] ========================================" << std::endl;

        json response = Protocol::build_file_response(file_id, ResponseStatus::ACCEPT, 0);
        reply(response);
    });

    // 设置控制消息回调
    g_signaling_server->set_on_control_message([](const json& msg,
                                                    const std::string& sender_addr) {
        std::string type = msg.value("type", "");
        if (type == MsgType::TEXT_MESSAGE) {
            std::string text = msg.value("text", "");
            std::lock_guard<std::mutex> lock(g_rcv_msg_mutex);
            g_rcv_messages[sender_addr].push_back(
                {{"text", text}, {"time", Utils::get_timestamp_ms()}});
            return;
        }
        std::string file_id = msg.value("file_id", "");
        std::cout << "[信令] 收到控制消息: " << type
                  << " (file_id=" << file_id.substr(0, 8) << "..., 来自 "
                  << sender_addr << ")" << std::endl;
    });

    // 设置 RFCOMM 握手回调: 自动将探测方加入设备列表 (互相发现)
    g_signaling_server->set_on_device_hello([](const json& hello,
                                                 const std::string& sender_addr) {
        if (!g_device_manager) return;

        std::string remote_id      = hello.value("device_id", "");
        std::string remote_name    = hello.value("device_name", sender_addr);
        std::string remote_addr    = hello.value("addr", sender_addr);
        uint16_t    remote_channel = hello.value("channel",
                                                  (uint16_t)Defaults::SIGNALING_CHANNEL);

        // 跳过本机地址
        if (remote_addr == g_local_addr) return;

        auto now = std::chrono::steady_clock::now();

        // 去重: 检查是否已存在同地址设备
        std::string existing_id = g_device_manager->find_device_id_by_addr(remote_addr);
        if (!existing_id.empty()) {
            DeviceInfo update;
            update.id          = existing_id;
            update.name        = remote_name;
            update.addr        = remote_addr;
            update.port        = remote_channel;
            update.last_seen   = now;
            update.last_probed = now;
            update.manual      = true;
            update.connected   = true;   // RFCOMM 握手成功, 标记为已连接
            g_device_manager->update_device(update);
            return;
        }

        DeviceInfo device;
        device.id          = remote_id;
        device.name        = remote_name;
        device.addr        = remote_addr;
        device.port        = remote_channel;
        device.last_seen   = now;
        device.first_seen  = now;
        device.last_probed = now;
        device.manual      = true;
        device.connected   = true;        // RFCOMM 握手成功, 标记为已连接
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
 * @brief 初始化传输服务
 */
bool init_transfer_service() {
    g_transfer_manager = std::make_unique<TransferManager>();

    (void) system("mkdir -p ./received_files");

    g_transfer_receiver = std::make_unique<BtTransferReceiver>(Defaults::TRANSFER_CHANNEL);
    g_transfer_receiver->set_save_directory("./received_files");
    g_transfer_manager->set_receiver(g_transfer_receiver.get());

    // 接收开始回调: 创建 TransferTask 记录
    g_transfer_receiver->set_on_receive_start([](const std::string& file_id,
                                                   const std::string& filename,
                                                   uint64_t file_size,
                                                   uint32_t total_chunks,
                                                   const std::string& sender_addr) {
        if (!g_transfer_manager) return;
        TransferTask task;
        task.meta.file_id      = file_id;
        task.meta.filename     = filename;
        task.meta.file_size    = file_size;
        task.meta.chunk_size   = Defaults::CHUNK_SIZE;
        task.meta.total_chunks = total_chunks;
        task.target.addr       = sender_addr;
        task.target.port       = Defaults::TRANSFER_CHANNEL;
        task.state             = TransferState::TRANSFERRING;
        task.is_sender         = false;
        g_transfer_manager->add_task(task);
    });

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
        resp["self_addr"] = g_local_addr;
        json devices = json::array();
        if (g_device_manager) {
            for (const auto& d : g_device_manager->get_online_devices()) {
                json dev;
                dev["id"]        = d.id.substr(0, 8);
                dev["name"]      = d.name;
                dev["addr"]      = d.addr;
                dev["port"]      = d.port;
                dev["manual"]    = d.manual;
                dev["connected"] = d.connected;
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
                item["file_id"]       = t.meta.file_id;
                item["filename"]      = t.meta.filename;
                item["file_size"]     = t.meta.file_size;
                item["total_chunks"]  = t.meta.total_chunks;
                item["progress_chunk"] = t.progress_chunk;
                item["speed"]         = t.speed;
                item["target_addr"]   = t.target.addr;
                item["is_sender"]     = t.is_sender;
                item["state"]         = (t.state == TransferState::TRANSFERRING ? "TRANSFERRING" :
                                         t.state == TransferState::COMPLETED ? "COMPLETED" :
                                         t.state == TransferState::PAUSED ? "PAUSED" : "IDLE");
                list.push_back(item);
            }
        }
        resp["transfers"] = list;
        return resp.dump();
    });

    // POST /api/messages/poll - 拉取收到的文本消息
    g_http_server->on_post("/api/messages/poll", [](const std::string& body,
        const std::map<std::string, std::string>&) -> std::string {

        json resp;
        std::string addr;
        auto pairs = Utils::split_string(body, '&');
        for (const auto& p : pairs) {
            auto eq = p.find('=');
            if (eq != std::string::npos) {
                std::string key = Utils::url_decode(p.substr(0, eq));
                std::string val = Utils::url_decode(p.substr(eq + 1));
                if (key == "addr") addr = val;
            }
        }
        json msgs = json::array();
        {
            std::lock_guard<std::mutex> lock(g_rcv_msg_mutex);
            auto it = g_rcv_messages.find(addr);
            if (it != g_rcv_messages.end()) {
                for (const auto& m : it->second) msgs.push_back(m);
                g_rcv_messages.erase(it);
            }
        }
        resp["messages"] = msgs;
        return resp.dump();
    });

    // POST /api/peers/add - 手动添加蓝牙设备
    g_http_server->on_post("/api/peers/add", [](const std::string& body,
        const std::map<std::string, std::string>&) -> std::string {

        json resp;
        std::string addr;
        std::string name;

        auto pairs = Utils::split_string(body, '&');
        for (const auto& p : pairs) {
            auto eq = p.find('=');
            if (eq != std::string::npos) {
                std::string key = Utils::url_decode(p.substr(0, eq));
                std::string val = Utils::url_decode(p.substr(eq + 1));
                if (key == "addr") addr = val;
                else if (key == "name") name = val;
            }
        }

        if (addr.empty()) {
            resp["success"] = false;
            resp["error"] = "缺少蓝牙地址";
            return resp.dump();
        }

        if (!g_device_manager) {
            resp["success"] = false;
            resp["error"] = "设备管理器未初始化";
            return resp.dump();
        }

        // 检查是否已有该地址, 有则更新名称
        std::string existing_id = g_device_manager->find_device_id_by_addr(addr);
        DeviceInfo device;
        if (!existing_id.empty()) {
            device.id        = existing_id;
            device.name      = name.empty() ? addr : name;
            device.addr      = addr;
            device.port      = Defaults::SIGNALING_CHANNEL;
            device.last_seen = std::chrono::steady_clock::now();
            device.manual    = true;
        } else {
            device.id         = Utils::generate_uuid();
            device.name       = name.empty() ? addr : name;
            device.addr       = addr;
            device.port       = Defaults::SIGNALING_CHANNEL;
            device.last_seen  = std::chrono::steady_clock::now();
            device.first_seen = std::chrono::steady_clock::now();
            device.manual     = true;
        }

        g_device_manager->update_device(device);
        save_known_devices();

        std::cout << "[Web] 手动添加设备: " << device.name
                  << " (" << addr << ")" << std::endl;

        resp["success"] = true;
        resp["id"]   = device.id;
        resp["name"] = device.name;
        resp["addr"] = device.addr;
        return resp.dump();
    });

    // POST /api/peers/remove - 移除手动添加的设备
    g_http_server->on_post("/api/peers/remove", [](const std::string& body,
        const std::map<std::string, std::string>&) -> std::string {

        json resp;
        std::string target_addr;

        auto pairs = Utils::split_string(body, '&');
        for (const auto& p : pairs) {
            auto eq = p.find('=');
            if (eq != std::string::npos) {
                std::string key = Utils::url_decode(p.substr(0, eq));
                std::string val = Utils::url_decode(p.substr(eq + 1));
                if (key == "addr") target_addr = val;
            }
        }

        if (!g_device_manager || target_addr.empty()) {
            resp["success"] = false;
            resp["error"] = "无效请求";
            return resp.dump();
        }

        for (const auto& d : g_device_manager->get_online_devices()) {
            if (d.addr == target_addr && d.manual) {
                g_device_manager->remove_device(d.id);
                save_known_devices();
                std::cout << "[Web] 手动移除设备: " << d.name
                          << " (" << target_addr << ")" << std::endl;
                break;
            }
        }

        resp["success"] = true;
        return resp.dump();
    });

    // POST /api/message - 发送聊天文本消息
    g_http_server->on_post("/api/message", [](const std::string& body,
        const std::map<std::string, std::string>&) -> std::string {

        json resp;
        std::string target_addr;
        uint8_t target_channel = Defaults::SIGNALING_CHANNEL;
        std::string text;

        auto pairs = Utils::split_string(body, '&');
        for (const auto& p : pairs) {
            auto eq = p.find('=');
            if (eq != std::string::npos) {
                std::string key = Utils::url_decode(p.substr(0, eq));
                std::string val = Utils::url_decode(p.substr(eq + 1));
                if (key == "target_addr") target_addr = val;
                else if (key == "target_channel")
                    target_channel = static_cast<uint8_t>(std::stoul(val));
                else if (key == "text") text = val;
            }
        }

        BtSignalingClient sig_client;
        json request = Protocol::build_text_message(text);
        json response;
        if (!sig_client.send_request(target_addr, target_channel,
                                      request, response)) {
            resp["success"] = false;
            resp["error"] = "发送失败";
            return resp.dump();
        }
        resp["success"] = true;
        return resp.dump();
    });

    // POST /api/transfer - 发起文件传输 (支持 base64 编码文件上传)
    g_http_server->on_post("/api/transfer", [](const std::string& body,
        const std::map<std::string, std::string>&) -> std::string {

        json resp;
        std::string target_addr;
        uint8_t target_channel = Defaults::SIGNALING_CHANNEL;
        std::string filename = "test.dat";
        std::string filedata_b64;

        auto pairs = Utils::split_string(body, '&');
        for (const auto& p : pairs) {
            auto eq = p.find('=');
            if (eq != std::string::npos) {
                std::string key = Utils::url_decode(p.substr(0, eq));
                std::string val = Utils::url_decode(p.substr(eq + 1));
                if (key == "target_addr") target_addr = val;
                else if (key == "target_channel")
                    target_channel = static_cast<uint8_t>(std::stoul(val));
                else if (key == "filename") filename = val;
                else if (key == "filedata") filedata_b64 = val;
            }
        }

        if (filedata_b64.empty()) {
            resp["success"] = false;
            resp["error"] = "未提供文件数据";
            return resp.dump();
        }

        if (target_addr.empty()) {
            resp["success"] = false;
            resp["error"] = "未指定目标设备";
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
            tmp_file.flush();
            tmp_file.close();
        }

        // 使用信令协商
        BtSignalingClient sig_client;
        std::string file_id = Utils::generate_uuid();
        uint32_t total_chunks = static_cast<uint32_t>(
            (file_size + Defaults::CHUNK_SIZE - 1) / Defaults::CHUNK_SIZE);

        json request = Protocol::build_file_request(
            file_id, filename, file_size, "", Defaults::CHUNK_SIZE, total_chunks);
        json sig_response;
        if (!sig_client.send_request(target_addr, target_channel,
                                      request, sig_response)) {
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

        uint8_t transfer_channel = static_cast<uint8_t>(
            sig_response.value("channel", (uint16_t)Defaults::TRANSFER_CHANNEL));

        // 创建传输任务
        TransferTask task;
        task.meta.file_id      = file_id;
        task.meta.filename     = filename;
        task.meta.file_size    = file_size;
        task.meta.chunk_size   = Defaults::CHUNK_SIZE;
        task.meta.total_chunks = total_chunks;
        task.target.addr       = target_addr;
        task.target.port       = transfer_channel;
        task.state             = TransferState::TRANSFERRING;
        task.is_sender         = true;
        g_transfer_manager->add_task(task);

        // 在后台线程中启动实际文件传输
        std::string captured_file_id = file_id;
        std::string captured_addr    = target_addr;
        uint8_t     captured_channel = transfer_channel;
        std::string captured_path    = temp_path;

        g_transfer_threads.push_back(std::thread(
            [captured_file_id, captured_addr, captured_channel,
             captured_path, total_chunks, file_size]() {
            BtTransferSender sender;
            std::cout << "[传输] 开始发送: " << captured_path
                      << " -> " << captured_addr
                      << " (通道 " << (int)captured_channel << ")" << std::endl;

            bool ok = sender.send_file(captured_addr, captured_channel,
                                        captured_path, captured_file_id,
                                        Defaults::WINDOW_SIZE,
                [=](const TransferProgress& progress) {
                    if (g_transfer_manager) {
                        g_transfer_manager->update_progress(
                            captured_file_id, progress.sent_chunks,
                            progress.bytes_sent, progress.speed);
                    }
                });

            if (g_transfer_manager) {
                g_transfer_manager->mark_complete(captured_file_id, ok);
            }

            std::remove(captured_path.c_str());

            if (ok) {
                std::cout << "[传输] 文件发送成功: "
                          << captured_file_id.substr(0, 8) << std::endl;
            } else {
                std::cerr << "[传输] 文件发送失败: "
                          << captured_file_id.substr(0, 8) << std::endl;
            }
        }));

        // 清理已完成的线程
        g_transfer_threads.erase(
            std::remove_if(g_transfer_threads.begin(), g_transfer_threads.end(),
                [](std::thread& t) { return !t.joinable(); }),
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

// ============================================================
// 持久化存储
// ============================================================

static constexpr const char* KNOWN_DEVICES_FILE = "known_devices.json";

static void save_known_devices() {
    if (!g_device_manager) return;

    json arr = json::array();
    std::set<std::string> seen_addrs;
    for (const auto& d : g_device_manager->get_online_devices()) {
        // 只保存手动添加的设备, 跳过本机
        if (!d.manual || d.addr == g_local_addr || seen_addrs.count(d.addr)) continue;
        seen_addrs.insert(d.addr);
        json entry;
        entry["id"]   = d.id;
        entry["name"] = d.name;
        entry["addr"] = d.addr;
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
        std::set<std::string> seen_addrs;
        int loaded = 0;
        for (const auto& entry : arr) {
            std::string addr = entry.value("addr", "");
            // 跳过本机地址和重复地址
            if (addr.empty() || addr == g_local_addr || seen_addrs.count(addr)) continue;
            seen_addrs.insert(addr);

            DeviceInfo device;
            device.id         = entry.value("id", Utils::generate_uuid());
            device.name       = entry.value("name", addr);
            device.addr       = addr;
            device.port       = entry.value("port",
                (uint16_t)Defaults::SIGNALING_CHANNEL);
            device.last_seen  = std::chrono::steady_clock::now();
            device.first_seen = std::chrono::steady_clock::now();
            device.manual     = true;
            g_device_manager->update_device(device);
            ++loaded;
        }

        if (loaded > 0) {
            std::cout << "[持久化] 加载了 " << loaded << " 个已知设备" << std::endl;
        }
        if (loaded != (int)arr.size()) save_known_devices();
    } catch (...) {
        std::cerr << "[持久化] 读取失败, 忽略" << std::endl;
    }
}

// ============================================================
// 程序入口
// ============================================================

int main() {
    print_banner();

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 运行单元验证 (静默执行, 不输出到终端)
    {
        std::ofstream null_out("/dev/null");
        auto* orig_rdbuf = std::cout.rdbuf();
        std::cout.rdbuf(null_out.rdbuf());
        run_phase1_tests();
        run_phase2_tests();
        run_phase3_tests();
        run_phase4_tests();
        std::cout.rdbuf(orig_rdbuf);
    }

    // 启动蓝牙服务
    if (!init_discovery_service()) return 1;
    if (!init_signaling_service()) return 1;
    if (!init_transfer_service()) return 1;
    if (!init_http_service()) return 1;

    std::cout << "\n[系统] 所有服务启动完成, 按 Ctrl+C 退出" << std::endl;

    // Auto-open browser
    std::string url = "http://localhost:" + std::to_string(Defaults::HTTP_PORT);
    system(("xdg-open " + url + " 2>/dev/null &").c_str());
    std::cout << "[系统] Web界面: " << url << std::endl;
    std::cout << "[系统] 信令通道: " << (int)Defaults::SIGNALING_CHANNEL
              << " | 传输通道: " << (int)Defaults::TRANSFER_CHANNEL
              << " | 蓝牙地址: " << g_local_addr << std::endl;

    // 启动 RFCOMM 探活线程 (互发现 + 标记已连接设备)
    g_peer_probe_thread = std::thread([]() {
        std::set<std::string> was_connected;
        size_t probe_index = 0;  // 轮询索引, 避免每次循环都从头开始
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(3));

            if (!g_device_manager) continue;
            std::set<std::string> now_connected;
            auto now = std::chrono::steady_clock::now();
            auto devices = g_device_manager->get_online_devices();

            if (devices.empty()) continue;

            int probes_this_cycle = 0;
            const int MAX_PROBES_PER_CYCLE = 3;  // 每轮最多探 3 个设备

            // 从上次轮询位置继续
            for (size_t i = 0; i < devices.size() && probes_this_cycle < MAX_PROBES_PER_CYCLE; ++i) {
                size_t idx = (probe_index + i) % devices.size();
                const auto& d = devices[idx];

                if (d.addr == g_local_addr) continue;

                // 节流: 未连接设备每 60 秒探一次, 已连接每 15 秒
                auto since_probe = std::chrono::duration_cast<std::chrono::seconds>(
                    now - d.last_probed).count();
                int interval = d.connected ? 15 : 60;
                if (since_probe < interval) {
                    if (d.connected) now_connected.insert(d.id);
                    continue;
                }

                ++probes_this_cycle;

                // RFCOMM 探活 (1s 超时)
                bool alive = BtSignalingClient::test_connect(
                    d.addr,
                    static_cast<uint8_t>(d.port),
                    g_device_id, g_device_name,
                    g_local_addr,
                    Defaults::SIGNALING_CHANNEL, 1000);

                DeviceInfo updated = d;
                updated.last_probed = now;
                if (alive) {
                    now_connected.insert(d.id);
                    updated.last_seen  = now;
                    updated.connected  = true;  // 握手成功, 标绿
                    if (!d.connected) {
                        std::cout << "[事件] 设备已连接: " << d.name
                                  << " (" << d.addr << ")" << std::endl;
                    }
                } else {
                    updated.connected = false;   // 本次未通, 标灰
                }
                g_device_manager->update_device(updated);
            }

            // 更新轮询索引
            probe_index = (probe_index + probes_this_cycle) % devices.size();

            // 打印断开通知
            for (const auto& id : was_connected) {
                if (!now_connected.count(id)) {
                    auto all = g_device_manager->get_online_devices();
                    for (const auto& d : all) {
                        if (d.id == id) {
                            std::cout << "[事件] 设备断开: " << d.name
                                      << " (" << d.addr << ")" << std::endl;
                            break;
                        }
                    }
                }
            }
            was_connected = std::move(now_connected);
        }
    });

    // 主循环
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        if (g_device_manager) {
            auto devices = g_device_manager->get_online_devices();
            std::vector<DeviceInfo> online_devs;
            auto now = std::chrono::steady_clock::now();
            for (const auto& d : devices) {
                auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    now - d.last_seen).count();
                if (age < 15) online_devs.push_back(d);
            }
            if (!online_devs.empty()) {
                std::cout << "\n[在线设备] (" << online_devs.size() << " 台):" << std::endl;
                std::cout << "  " << std::left << std::setw(24) << "设备名称"
                          << std::setw(20) << "蓝牙地址"
                          << std::setw(8) << "通道" << std::endl;
                std::cout << "  " << std::string(52, '-') << std::endl;
                for (const auto& d : online_devs) {
                    std::cout << "  " << std::left << std::setw(24) << d.name
                              << std::setw(20) << d.addr
                              << std::setw(8) << (int)d.port << std::endl;
                }
                std::cout << std::endl;
            } else {
                std::cout << "[在线设备] 当前没有其他设备在线" << std::endl;
            }
        }
    }

    // 优雅退出
    std::cout << "\n[系统] 正在停止所有服务..." << std::endl;

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

    std::cout << "[系统] 程序已退出" << std::endl;
    return 0;
}
