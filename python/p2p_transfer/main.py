#!/usr/bin/env python3
"""
P2P 文件传输工具 — 主入口

程序启动流程：
1. 输出横幅 Banner
2. 运行 4 阶段内联单元测试（42 项）
3. 初始化全部服务：
   - DeviceManager（设备列表 + 超时清理）
   - DeviceDiscovery（UDP 组播设备发现）
   - SignalingServer（TCP 信令服务，端口 8889）
   - TransferReceiver（文件接收服务，端口 8890）
   - HttpServer（Flask Web UI + REST API，端口 8891）
4. 启动对等探测线程（每 5 秒对所有已知设备执行 ECDH 握手）
5. 进入主循环（每 5 秒打印在线设备表）
6. 收到 SIGINT/SIGTERM 后优雅关闭所有服务

单元测试 4 阶段：
- 第 1 阶段：UUID、MD5、格式化、协议消息构造
- 第 2 阶段：DeviceManager 增删改、超时回收、持久化
- 第 3 阶段：信令服务端 + 客户端往返、ECDH 握手
- 第 4 阶段：分块序列化、FileChunkIO 本地 I/O、端到端传输

运行方式：
    python -u python/p2p_transfer/main.py

环境要求：
    conda activate alg
    pip install cryptography flask
"""

import os
import sys
import time
import json
import signal
import threading
import socket

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from p2p_transfer.common.types import (
    DISCOVERY_PORT,
    SIGNALING_PORT,
    TRANSFER_PORT,
    HTTP_PORT,
    DEVICE_TIMEOUT,
    DeviceInfo,
    TransferState,
    TransferTask,
    FileMeta,
    CHUNK_SIZE,
    MsgType,
    Status,
)
from p2p_transfer.common.utils import (
    generate_uuid,
    md5_data,
    md5_file,
    format_file_size,
    format_speed,
    generate_ecdh_keypair,
    compute_ecdh_shared,
    aes_gcm_encrypt,
    aes_gcm_decrypt,
    compress_data,
    decompress_data,
    get_time_string,
    b64_encode,
    b64_decode,
)
from p2p_transfer.common.platform import select_local_ip, enable_bbr
from p2p_transfer.common.peer_key import PeerKey
from p2p_transfer.common.protocol import (
    send_json_message,
    recv_json_message,
    send_json_message_raw,
    recv_json_message_raw,
    build_device_broadcast,
    build_device_hello,
    build_device_hello_ack,
    build_text_message,
    build_file_request,
    build_file_response,
    build_chunk_ack,
    build_control_message,
    build_file_range,
    build_range_done,
    build_range_ack,
    build_device_offline,
)
from p2p_transfer.discovery.device_discovery import DeviceDiscovery
from p2p_transfer.discovery.device_manager import DeviceManager
from p2p_transfer.signaling.signaling_server import SignalingServer
from p2p_transfer.signaling.signaling_client import SignalingClient
from p2p_transfer.transfer.transfer_receiver import TransferReceiver
from p2p_transfer.transfer.transfer_manager import TransferManager
from p2p_transfer.transfer.chunk import (
    serialize_chunk,
    deserialize_chunk,
    CHUNK_SENTINEL_INDEX,
)
from p2p_transfer.transfer.file_io import FileChunkIO
from p2p_transfer.web.http_server import HttpServer

# ─── 全局状态 ─────────────────────────────────────────────────────────────────
# 主循环运行标志，由信号处理器置为 False 触发优雅关闭
g_running = True
# 前端静态文件目录的绝对路径
STATIC_DIR = os.path.join(os.path.dirname(__file__), "web", "static")


# ═══════════════════════════════════════════════════════════════════════════════
# 内联单元测试（4 阶段，42 项）
# ═══════════════════════════════════════════════════════════════════════════════

def print_banner():
    """输出程序横幅"""
    print("=" * 60)
    print("  P2P File Transfer Tool (Python)")
    print("=" * 60)


def run_unit_tests():
    """运行全部内联单元测试，返回 True 表示全部通过"""
    print("[TESTS] Running unit tests...")
    passed = 0
    failed = 0

    def check(name, cond):
        """辅助函数：记录单条测试结果"""
        nonlocal passed, failed
        if cond:
            passed += 1
        else:
            failed += 1
            print(f"  FAIL: {name}")

    # ── 第 1 阶段：UUID、格式化、协议消息构造 ──
    uid = generate_uuid()
    check("UUID not empty", len(uid) > 0)
    check("UUID has 4 dashes", uid.count("-") == 4)           # UUID v4 格式：8-4-4-4-12

    chk = format_file_size(1234567)
    check("format_file_size", "MB" in chk)

    spd = format_speed(1234567)
    check("format_speed", "/s" in spd)

    msg = build_device_broadcast("id1", "test", "1.2.3.4", 8889, 12345.0)
    check("build_broadcast", msg["type"] == "DEVICE_BROADCAST")
    check("build_broadcast_ip", msg["ip"] == "1.2.3.4")
    check("build_broadcast_port", msg["port"] == 8889)
    check("build_broadcast_ts", msg["timestamp"] == 12345.0)

    msg = build_device_offline("id1")
    check("build_offline", msg["type"] == "DEVICE_OFFLINE")

    msg = build_text_message("Hello!")
    check("build_text", msg["text"] == "Hello!")

    msg = build_file_response("fid", Status.ACCEPT, 0)
    check("build_file_response", msg["status"] == "ACCEPT")

    msg = build_chunk_ack("fid", 42)
    check("build_chunk_ack", msg["max_contiguous_chunk"] == 42)

    msg = build_file_range("fid", "test.dat", 100000, 2, 65536, 0, 0, 0, 2, "zlib")
    check("build_file_range_type", msg["type"] == "FILE_RANGE")
    check("build_file_range_compression", msg["compression"] == "zlib")

    # ── 第 2 阶段：DeviceManager 增删/超时/持久化（无网络） ──
    dm = DeviceManager()
    online = []
    offline = []

    def on_online(d):
        online.append(d.id)

    def on_offline(d):
        offline.append(d.id)

    dm.set_on_device_online(on_online)
    dm.set_on_device_offline(on_offline)

    d1 = DeviceInfo(id="d1", name="Dev1", ip="10.0.0.1", port=8889)
    d2 = DeviceInfo(id="d2", name="Dev2", ip="10.0.0.2", port=8889)
    dm.add_device(d1)
    dm.add_device(d2)

    check("dm_count", len(dm.get_all_devices()) == 2)
    check("dm_online1", "d1" in online)
    check("dm_online2", "d2" in online)

    found = dm.find_device_id_by_ip("10.0.0.1")
    check("dm_find_by_ip", found == "d1")

    dm.remove_device("d1")
    check("dm_remove", len(dm.get_all_devices()) == 1)
    check("dm_offline", "d1" in offline)

    # 手动设备持久化测试
    d3 = DeviceInfo(id="d3", name="Manual", ip="192.168.1.1", port=8889, manual=True)
    dm.add_device(d3)
    dm.save_known_devices("/tmp/test_known.json", ["127.0.0.1"])
    check("dm_save", os.path.exists("/tmp/test_known.json"))
    dm2 = DeviceManager()
    dm2.load_known_devices("/tmp/test_known.json", ["127.0.0.1"])
    devs = dm2.get_all_devices()
    check("dm_load_manual", any(d.ip == "192.168.1.1" for d in devs))
    os.unlink("/tmp/test_known.json")

    # ── 第 3 阶段：信令服务端 + 客户端往返、ECDH 握手 ──
    print("  [Phase 3: Signaling round-trip...]")
    pub_a, priv_a = generate_ecdh_keypair()
    pub_b, priv_b = generate_ecdh_keypair()

    ack_state = threading.Event()
    ack_received = threading.Event()
    received_pub = [None]

    def device_hello_cb(msg, sender_ip):
        """信令服务端 DEVICE_HELLO 回调：记录远端公钥"""
        received_pub[0] = msg.get("public_key", "")
        ack_received.set()

    def text_msg_cb(msg, sender_ip):
        """信令服务端 TEXT_MESSAGE 回调"""
        ack_state.set()

    # 启动本地信令测试服务器（端口 18889，避免与生产端口冲突）
    server = SignalingServer(port=18889, ecdh_public_key=pub_b)
    server.set_device_hello_callback(device_hello_cb)
    server.set_text_msg_callback(text_msg_cb)
    server.start()
    time.sleep(0.3)

    # ECDH 握手测试
    ok = SignalingClient.test_connect(
        "127.0.0.1", 18889, "test-id", "test", "127.0.0.1",
        pub_a, priv_a, timeout_ms=3000,
    )
    check("sig_hello_handshake", ok and ack_received.wait(2))

    # 验证双方 ECDH 共享密钥一致
    shared_b = compute_ecdh_shared(priv_b, received_pub[0] or "")
    check("sig_ecdh_server", shared_b is not None and len(shared_b) == 32)
    shared_a = PeerKey.get("127.0.0.1")
    check("sig_ecdh_agree", shared_a == shared_b)

    # 文本消息中继测试
    text_msg = build_text_message("Test relay")
    text_msg["sender_id"] = "sender1"
    text_msg["sender_name"] = "Sender"
    text_msg["sender_ip"] = "127.0.0.1"
    SignalingClient.send_request("127.0.0.1", 18889, text_msg, timeout_ms=2000)
    check("sig_text_relay", ack_state.wait(2))

    server.stop()

    # ── 第 4 阶段：分块序列化、FileChunkIO、端到端传输 ──
    print("  [Phase 4: Chunk + File I/O + E2E transfer...]")

    # 分块序列化/反序列化测试
    chunk_data = os.urandom(32768)
    ser = serialize_chunk(0, 2, "test-file-id", chunk_data)
    result = deserialize_chunk(ser)
    check("chunk_serde", result is not None)
    if result:
        ci, tc, fid, data = result
        check("chunk_index", ci == 0)
        check("chunk_total", tc == 2)
        check("chunk_file_id", fid == "test-file-id")
        check("chunk_data", data == chunk_data)

    # 小数据分块测试
    ser2 = serialize_chunk(1, 2, "test-file-id", b"smalldata")
    result2 = deserialize_chunk(ser2)
    check("chunk_serde2", result2 is not None)
    if result2:
        check("chunk_index2", result2[0] == 1)
        check("chunk_data2", result2[3] == b"smalldata")

    # FileChunkIO 发送端测试（100KB 文件 = 2 块）
    test_file = "/tmp/p2p_test_file.bin"
    test_data = b"A" * 100000                     # 可压缩数据（zlib 后将远小于 64KB）
    with open(test_file, "wb") as f:
        f.write(test_data)

    io_send = FileChunkIO(test_file, is_sender=True)
    check("fileio_sender_chunks", io_send.total_chunks == 2)  # 100000/65536 = 1.53 → 2

    c0 = io_send.read_chunk(0)
    c1 = io_send.read_chunk(1)
    check("fileio_sender_c0", len(c0) == 65536)    # 第一块 = 65536
    check("fileio_sender_c1", len(c1) == 34464)    # 最后一块 = 100000 - 65536
    io_send.close()

    # FileChunkIO 接收端测试
    tmp_file = test_file + ".tmp"
    with open(tmp_file, "wb") as f:
        f.truncate(100000)                          # 预分配 100KB

    io_recv = FileChunkIO(test_file, is_sender=False)
    io_recv.set_total_size(100000, 2)
    io_recv.write_chunk(0, c0)
    io_recv.write_chunk(1, c1)
    check("fileio_recv_all", io_recv.all_chunks_received())
    io_recv.commit_received_file()

    with open(test_file, "rb") as f:
        readback = f.read()
    check("fileio_readback", readback == test_data)

    os.unlink(test_file)
    if os.path.exists(tmp_file):
        os.unlink(tmp_file)

    # 端到端网络传输测试（本地回环，端口 18890）
    print("  [Phase 4b: E2E transfer on localhost:18890...]")

    test_file2 = "/tmp/p2p_e2e_test.bin"
    with open(test_file2, "wb") as f:
        f.write(test_data)
    recv_dir = "/tmp/p2p_test_recv"
    os.makedirs(recv_dir, exist_ok=True)

    # 启动接收端
    receiver = TransferReceiver(port=18890, save_dir=recv_dir)
    complete_event = threading.Event()
    recv_file_id = [None]
    recv_path = [None]

    def on_complete(fid, path, size):
        recv_file_id[0] = fid
        recv_path[0] = path
        complete_event.set()

    receiver.set_on_complete(on_complete)
    receiver.start()
    time.sleep(0.3)

    # 发送文件（从 main.py 导入 TransferSender）
    from p2p_transfer.transfer.transfer_sender import TransferSender

    sender = TransferSender()
    fid = generate_uuid()
    ok = sender.send_file("127.0.0.1", 18890, test_file2, fid, num_threads=2)
    check("e2e_send_ok", ok)

    completed = complete_event.wait(timeout=10)
    check("e2e_recv_complete", completed)

    if completed and recv_path[0]:
        with open(recv_path[0], "rb") as f:
            recv_data = f.read()
        check("e2e_data_match", recv_data == test_data)

    receiver.stop()
    os.unlink(test_file2)

    import shutil
    shutil.rmtree(recv_dir, ignore_errors=True)

    print(f"\n[TESTS] Results: {passed} passed, {failed} failed")
    return failed == 0


# ═══════════════════════════════════════════════════════════════════════════════
# 服务初始化
# ═══════════════════════════════════════════════════════════════════════════════

def init_services():
    """初始化全部 P2P 服务组件，返回服务字典

    初始化顺序：
    1. 确定本机 IP 和设备身份（UUID、名称、ECDH 密钥对）
    2. 启动 DeviceManager（设备列表）
    3. 启动 DeviceDiscovery（UDP 组播发现）
    4. 创建 TransferManager（传输任务管理）
    5. 启动 SignalingServer（TCP 信令服务）
    6. 启动 TransferReceiver（文件接收服务）
    7. 启动 HttpServer（Web UI + REST API）

    Returns:
        包含所有服务实例的字典
    """
    # ── 本机身份 ──
    local_ip = select_local_ip()
    device_id = generate_uuid()
    # 设备名格式：PyP2P-{IP 最后一段}
    device_name = f"PyP2P-{local_ip.split('.')[-1]}"
    public_key, private_key = generate_ecdh_keypair()

    print(f"\n[INIT] Device: {device_name}")
    print(f"[INIT] UUID: {device_id}")
    print(f"[INIT] Local IP: {local_ip}")

    # ── 设备发现 ──
    dm = DeviceManager()
    dm.set_on_device_online(lambda d: print(f"  [ONLINE] {d.name} ({d.ip})"))
    dm.set_on_device_offline(lambda d: print(f"  [OFFLINE] {d.name} ({d.ip})"))
    dm.start()
    # 加载已知手动设备列表
    dm.load_known_devices("known_devices.json", [local_ip])

    dd = DeviceDiscovery(device_id, device_name, port=DISCOVERY_PORT)
    # 收到广播消息后更新设备列表
    dd.set_on_device_found(lambda info: dm.update_device(info))
    dd.start()

    # ── 传输管理 ──
    tm = TransferManager()

    # ── 信令服务回调 ──

    def file_request_cb(msg, sender_ip, sock):
        """处理文件传输请求：自动接受并通知"""
        from p2p_transfer.common.protocol import send_json_message

        file_id = msg.get("file_id", "")
        filename = msg.get("filename", "")
        file_size = msg.get("file_size", 0)

        # 自动接受所有文件传输请求
        response = {"type": "FILE_RESPONSE", "file_id": file_id, "status": "ACCEPT"}
        send_json_message(sock, response, sender_ip)
        print(f"  [ACCEPT] Receiving {filename} ({format_file_size(file_size)})")

    def device_hello_cb(msg, sender_ip):
        """处理设备探测握手：提取 ECDH 公钥并计算共享密钥"""
        sender_name = msg.get("device_name", "")
        sender_id = msg.get("device_id", "")
        remote_pub = msg.get("public_key", "")

        # 若对方提供了公钥，执行 ECDH 密钥交换
        if remote_pub:
            shared = compute_ecdh_shared(private_key, remote_pub)
            if shared and len(shared) == 32:
                PeerKey.store(sender_ip, shared)

        # 更新设备列表
        info = DeviceInfo(
            id=sender_id,
            name=sender_name,
            ip=sender_ip,
            port=SIGNALING_PORT,
            last_seen=time.time(),
        )
        dm.update_device(info)

    def text_msg_cb(msg, sender_ip):
        """处理文本消息：打印并存入接收缓冲区供前端轮询"""
        sender_name = msg.get("sender_name", "Unknown")
        text = msg.get("text", "")
        print(f"  [MSG] {sender_name}: {text}")

        # 存入 HTTP 服务器的消息缓冲区
        msg_to_store = {
            "sender_name": sender_name,
            "sender_ip": msg.get("sender_ip", sender_ip),
            "text": text,
            "timestamp": time.time(),
        }
        http_server.add_received_message(
            msg.get("sender_ip", sender_ip), msg_to_store
        )

    # ── 启动信令服务（端口 8889） ──
    ss = SignalingServer(port=SIGNALING_PORT, ecdh_public_key=public_key)
    ss.set_file_request_callback(file_request_cb)
    ss.set_device_hello_callback(device_hello_cb)
    ss.set_text_msg_callback(text_msg_cb)
    ss.start()

    # ── 启动文件接收服务（端口 8890） ──
    tr = TransferReceiver(port=TRANSFER_PORT, save_dir="./received_files")

    def on_transfer_complete(fid, path, size):
        """传输完成回调：通知 TransferManager"""
        print(f"  [RECV COMPLETE] {os.path.basename(path)} ({format_file_size(size)})")
        tm.mark_complete(fid, True)

    tr.set_on_complete(on_transfer_complete)
    tr.start()

    # ── 启动 HTTP 服务（端口 8891） ──
    # 注意：http_server 变量在 text_msg_cb 闭包中被引用，
    # 必须在信令服务回调注册之后但在 text_msg_cb 被调用之前定义
    http_server = HttpServer(
        port=HTTP_PORT,
        static_dir=STATIC_DIR,
        device_manager=dm,
        transfer_manager=tm,
        local_ip=local_ip,
        device_id=device_id,
        device_name=device_name,
    )
    http_server.start()

    print(f"\n[READY] HTTP: http://{local_ip}:{HTTP_PORT}")
    print(f"[READY] Discovery: UDP {DISCOVERY_PORT}")
    print(f"[READY] Signaling: TCP {SIGNALING_PORT}")
    print(f"[READY] Transfer: TCP {TRANSFER_PORT}")
    print()

    return {
        "dm": dm,
        "dd": dd,
        "ss": ss,
        "tr": tr,
        "tm": tm,
        "http": http_server,
        "public_key": public_key,
        "private_key": private_key,
        "device_id": device_id,
        "device_name": device_name,
        "local_ip": local_ip,
    }


# ═══════════════════════════════════════════════════════════════════════════════
# 后台线程：对等探测 + 主循环
# ═══════════════════════════════════════════════════════════════════════════════

def probe_loop(services):
    """对等探测线程：每 5 秒对每个已知设备执行 ECDH 握手

    探测流程：
    1. 遍历 DeviceManager 中的所有设备（跳过本机）
    2. 对每个设备调用 test_connect → DEVICE_HELLO → DEVICE_HELLO_ACK → 计算共享密钥
    3. 成功建立密钥后，后续所有消息（文本、文件分块）将自动加密

    此线程持续运行直到 g_running 被信号处理器置为 False。
    """
    dm = services["dm"]
    public_key = services["public_key"]
    private_key = services["private_key"]
    device_id = services["device_id"]
    device_name = services["device_name"]
    local_ip = services["local_ip"]

    while g_running:
        time.sleep(5)
        if not g_running:
            break

        devices = dm.get_all_devices()
        for dev in devices:
            if dev.ip == local_ip:
                continue                                     # 跳过本机
            # 执行 ECDH 探测握手
            ok = SignalingClient.test_connect(
                dev.ip, dev.port or SIGNALING_PORT,
                device_id, device_name, local_ip,
                public_key, private_key, timeout_ms=3000,
            )
            if ok:
                dm.update_device(dev)                         # 刷新 last_seen


def main_loop(services):
    """主循环：每 5 秒打印在线设备列表

    在线判定：last_seen 距今 < 15 秒
    KEY 状态：已建立 ECDH 共享密钥
    """
    dm = services["dm"]
    print("  Online devices:")
    while g_running:
        time.sleep(5)
        if not g_running:
            break

        devices = dm.get_all_devices()
        now = time.time()
        online = [d for d in devices if (now - d.last_seen) < 15]

        print(f"\n[{get_time_string()}] Online: {len(online)} device(s)")
        for d in online:
            age = int(now - d.last_seen)
            key_status = "KEY" if PeerKey.has(d.ip) else "nokey"
            print(f"  {d.name:<20} {d.ip:<16} {age}s ago  {key_status}")


# ═══════════════════════════════════════════════════════════════════════════════
# 优雅关闭
# ═══════════════════════════════════════════════════════════════════════════════

def shutdown(services):
    """按依赖顺序优雅关闭所有服务

    关闭顺序（与启动顺序相反）：
    DeviceDiscovery → SignalingServer → TransferReceiver → HttpServer → DeviceManager
    """
    print("\n[SHUTDOWN] Stopping services...")
    services["dd"].stop()       # 1. 停止 UDP 组播（发送 DEVICE_OFFLINE）
    services["ss"].stop()       # 2. 停止信令服务（关闭 TCP 监听）
    services["tr"].stop()       # 3. 停止文件接收（关闭 TCP 监听）
    services["http"].stop()     # 4. 停止 HTTP 服务（关闭 Flask）
    services["dm"].stop()       # 5. 停止设备管理（清理后台线程）
    print("[SHUTDOWN] Done.")


def signal_handler(signum, frame):
    """SIGINT / SIGTERM 信号处理器"""
    global g_running
    g_running = False
    print(f"\n[SHUTDOWN] Signal {signum} received, shutting down...")


# ═══════════════════════════════════════════════════════════════════════════════
# 主函数
# ═══════════════════════════════════════════════════════════════════════════════

def main():
    """程序主入口"""
    global g_running

    print_banner()

    # 第一步：运行单元测试
    if not run_unit_tests():
        print("[ERROR] Unit tests failed. Exiting.")
        sys.exit(1)

    # 第二步：初始化全部服务
    services = init_services()

    # 第三步：注册信号处理器
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # 第四步：启动对等探测后台线程
    probe_thread = threading.Thread(
        target=probe_loop, args=(services,), daemon=True,
    )
    probe_thread.start()

    # 第五步：进入主循环（按 Ctrl+C 退出）
    try:
        main_loop(services)
    except KeyboardInterrupt:
        g_running = False

    # 第六步：优雅关闭
    g_running = False
    shutdown(services)

    if probe_thread.is_alive():
        probe_thread.join(timeout=2)


if __name__ == "__main__":
    main()
