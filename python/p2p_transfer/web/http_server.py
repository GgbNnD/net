"""
HTTP 服务器与 REST API 模块

基于 Flask + werkzeug 的轻量 HTTP 服务器，提供：
- 静态前端文件服务（index.html, app.js, style.css）
- 7 个 REST API 端点
- CORS 跨域支持（Access-Control-Allow-Origin: *）
- 接收消息缓冲区（供前端轮询）

API 端点清单：
┌─────────┬─────────────────────┬─────────────────────────────────────────┐
│ Method  │ Path                │ 功能                                    │
├─────────┼─────────────────────┼─────────────────────────────────────────┤
│ GET     │ /api/devices        │ 在线设备列表 + self_name/self_ip        │
│ GET     │ /api/transfers      │ 传输任务列表 + 进度                     │
│ POST    │ /api/messages/poll  │ 轮询指定 IP 的接收消息（取出后清空）     │
│ POST    │ /api/peers/add      │ 手动添加设备（持久化到 known_devices）  │
│ POST    │ /api/peers/remove   │ 手动移除设备                            │
│ POST    │ /api/message        │ 发送文本消息到指定设备                   │
│ POST    │ /api/transfer       │ 发送 Base64 编码文件到指定设备           │
└─────────┴─────────────────────┴─────────────────────────────────────────┘

线程模型：
- 主线程通过 Flask + werkzeug make_server 启动守护线程
- 每个 HTTP 请求由 Flask 的 threaded=True 处理
"""

import os
import json
import base64
import threading
import time
from flask import Flask, request, jsonify, send_from_directory

from p2p_transfer.common.types import (
    HTTP_PORT,
    SIGNALING_PORT,
    TransferState,
    TransferTask,
    FileMeta,
    DeviceInfo,
    CHUNK_SIZE,
    TRANSFER_PORT,
)
from p2p_transfer.common.utils import generate_uuid, get_file_size
from p2p_transfer.signaling.signaling_client import SignalingClient
from p2p_transfer.transfer.transfer_sender import TransferSender
from p2p_transfer.common.protocol import build_text_message
from p2p_transfer.common.peer_key import PeerKey


class HttpServer:
    """Flask 封装的 HTTP 服务器

    通过 werkzeug.serving.make_server 实现可控的启停。
    """

    def __init__(
        self,
        port: int = HTTP_PORT,
        static_dir: str = "",
        device_manager=None,
        transfer_manager=None,
        local_ip: str = "",
        device_id: str = "",
        device_name: str = "",
    ):
        """初始化 HTTP 服务器

        Args:
            port: HTTP 监听端口（默认 8891）
            static_dir: 前端静态文件目录的绝对路径
            device_manager: DeviceManager 实例（供 API 查询设备）
            transfer_manager: TransferManager 实例（供 API 查询任务）
            local_ip: 本机 IP（API 返回用）
            device_id: 本机 UUID（消息发送方标识）
            device_name: 本机显示名称（API + 消息用）
        """
        self._port = port
        self._static_dir = static_dir
        self._device_manager = device_manager
        self._transfer_manager = transfer_manager
        self._local_ip = local_ip
        self._device_id = device_id
        self._device_name = device_name

        # 接收消息缓冲区：IP → 消息列表（供前端轮询）
        self._rcv_messages: dict[str, list[dict]] = {}
        self._rcv_lock = threading.Lock()

        # 服务管理
        self._thread: threading.Thread | None = None
        self._server = None

        # 创建 Flask 应用并注册路由
        self._app = Flask(__name__)
        self._app.config["SECRET_KEY"] = os.urandom(24).hex()
        self._register_routes()

    def start(self):
        """启动 HTTP 服务器（在 daemon 线程中运行）"""
        self._server = self._make_server()
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()

    def stop(self):
        """停止 HTTP 服务器"""
        if self._server:
            self._server.shutdown()
        if self._thread:
            self._thread.join(timeout=3)

    def add_received_message(self, sender_ip: str, msg: dict):
        """向接收消息缓冲区添加一条消息，供前端轮询取出"""
        with self._rcv_lock:
            if sender_ip not in self._rcv_messages:
                self._rcv_messages[sender_ip] = []
            self._rcv_messages[sender_ip].append(msg)

    def _make_server(self):
        """创建 werkzeug 服务器实例（threaded=True 支持并发请求）"""
        from werkzeug.serving import make_server

        return make_server("0.0.0.0", self._port, self._app, threaded=True)

    def _register_routes(self):
        """注册所有 Flask 路由和 CORS 处理器"""
        app = self._app
        static_dir = self._static_dir

        # ── CORS 全局后处理 ──
        @app.after_request
        def add_cors(response):
            """为所有响应添加 CORS 跨域头"""
            response.headers["Access-Control-Allow-Origin"] = "*"
            response.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS"
            response.headers["Access-Control-Allow-Headers"] = "Content-Type"
            return response

        # ── 静态前端文件 ──
        @app.route("/", methods=["GET", "OPTIONS"])
        def index():
            return send_from_directory(static_dir, "index.html")

        @app.route("/<path:path>", methods=["GET", "OPTIONS"])
        def static_files(path):
            """通配路由：服务所有静态资源（JS, CSS 等）"""
            if request.method == "OPTIONS":
                return jsonify({})
            return send_from_directory(static_dir, path)

        # ── REST API ──

        @app.route("/api/devices", methods=["GET", "OPTIONS"])
        def api_devices():
            """获取在线设备列表

            每个设备包含 online 字段（最后活跃时间 < 15 秒视为在线）
            和 manual 标志（手动添加的设备永不过期）。
            """
            if request.method == "OPTIONS":
                return jsonify({})
            devices = []
            now = time.time()
            dm = self._device_manager
            if dm:
                for dev in dm.get_all_devices():
                    online = (now - dev.last_seen) < 15
                    devices.append(
                        {
                            "id": dev.id,
                            "name": dev.name,
                            "ip": dev.ip,
                            "port": dev.port,
                            "last_seen": dev.last_seen,
                            "online": online,
                            "manual": dev.manual,
                        }
                    )
            return jsonify(
                {
                    "devices": devices,
                    "self_name": self._device_name,
                    "self_ip": self._local_ip,
                }
            )

        @app.route("/api/transfers", methods=["GET", "OPTIONS"])
        def api_transfers():
            """获取传输任务列表及进度

            返回每个任务的文件名、目标、状态、进度、速度等信息。
            """
            if request.method == "OPTIONS":
                return jsonify({})
            tasks = []
            tm = self._transfer_manager
            if tm:
                for t in tm.get_all_tasks():
                    progress = 0.0
                    if t.meta.total_chunks > 0:
                        progress = t.progress_chunk / t.meta.total_chunks
                    tasks.append(
                        {
                            "file_id": t.meta.file_id,
                            "filename": t.meta.filename,
                            "file_size": t.meta.file_size,
                            "target_ip": t.target.ip,
                            "target_name": t.target.name,
                            "is_sender": t.is_sender,
                            "state": t.state.value,
                            "progress": progress,
                            "bytes_sent": t.bytes_sent,
                            "speed": t.speed,
                        }
                    )
            return jsonify({"transfers": tasks})

        @app.route("/api/messages/poll", methods=["POST", "OPTIONS"])
        def api_messages_poll():
            """轮询指定 IP 的接收消息

            取出后立即清空该 IP 的消息缓冲区（消费模型）。
            参数：ip（表单或 JSON 中的 IP 地址）
            """
            if request.method == "OPTIONS":
                return jsonify({})
            ip = request.form.get("ip", "")
            if not ip:
                data = request.get_json(silent=True) or {}
                ip = data.get("ip", "")
            messages = []
            with self._rcv_lock:
                if ip in self._rcv_messages:
                    messages = self._rcv_messages[ip]
                    self._rcv_messages[ip] = []
            return jsonify({"messages": messages})

        @app.route("/api/peers/add", methods=["POST", "OPTIONS"])
        def api_peers_add():
            """手动添加设备

            按 IP 去重后创建 manual=True 的 DeviceInfo，
            并保存到 known_devices.json 持久化。
            参数：ip, name（可选）
            """
            if request.method == "OPTIONS":
                return jsonify({})
            ip = request.form.get("ip", "")
            name = request.form.get("name", "Unknown")
            if not ip:
                data = request.get_json(silent=True) or {}
                ip = data.get("ip", "")
                name = data.get("name", name)

            if not ip:
                return jsonify({"ok": False, "error": "ip required"}), 400

            dm = self._device_manager
            if dm:
                existing = dm.find_device_id_by_ip(ip)
                if existing:
                    return jsonify({"ok": True, "existed": True})

                import uuid

                dev = DeviceInfo(
                    id=str(uuid.uuid4()),
                    name=name,
                    ip=ip,
                    port=SIGNALING_PORT,
                    last_seen=time.time(),
                    first_seen=time.time(),
                    manual=True,
                )
                dm.add_device(dev)

                # 持久化保存
                known_path = "known_devices.json"
                dm.save_known_devices(known_path, [self._local_ip])

            return jsonify({"ok": True})

        @app.route("/api/peers/remove", methods=["POST", "OPTIONS"])
        def api_peers_remove():
            """手动移除设备

            移除设备、删除 ECDH 密钥、更新 known_devices.json。
            参数：ip
            """
            if request.method == "OPTIONS":
                return jsonify({})
            ip = request.form.get("ip", "")
            if not ip:
                data = request.get_json(silent=True) or {}
                ip = data.get("ip", "")

            if not ip:
                return jsonify({"ok": False, "error": "ip required"}), 400

            dm = self._device_manager
            if dm:
                PeerKey.remove(ip)                      # 清除该 IP 的 ECDH 密钥
                dm.remove_by_ip(ip)
                known_path = "known_devices.json"
                dm.save_known_devices(known_path, [self._local_ip])

            return jsonify({"ok": True})

        @app.route("/api/message", methods=["POST", "OPTIONS"])
        def api_message():
            """发送文本消息到指定设备

            通过 SignalingClient 发起到目标设备信令端口的短连接，
            自动加密（若有 ECDH 密钥）或明文发送。
            参数：target_ip, text
            """
            if request.method == "OPTIONS":
                return jsonify({})
            target_ip = request.form.get("target_ip", "")
            text = request.form.get("text", "")
            if not target_ip:
                data = request.get_json(silent=True) or {}
                target_ip = data.get("target_ip", "")
                text = data.get("text", "")

            if not target_ip or not text:
                return jsonify(
                    {"ok": False, "error": "target_ip and text required"}
                ), 400

            msg = build_text_message(text)
            msg["sender_id"] = self._device_id
            msg["sender_name"] = self._device_name
            msg["sender_ip"] = self._local_ip

            response = SignalingClient.send_request(
                target_ip, SIGNALING_PORT, msg, timeout_ms=5000
            )
            return jsonify(
                {"ok": response is not None, "response": response}
            )

        @app.route("/api/transfer", methods=["POST", "OPTIONS"])
        def api_transfer():
            """发起文件传输

            处理流程：
            1. 解析 Base64 文件数据 → 写入 /tmp/p2p_send/
            2. 创建 TransferTask 并注册到 TransferManager
            3. 在后台线程中启动 TransferSender
            4. 返回 file_id 供前端轮询进度

            参数：target_ip, filename, filedata（Base64 编码的文件内容）
            """
            if request.method == "OPTIONS":
                return jsonify({})
            target_ip = request.form.get("target_ip", "")
            filename = request.form.get("filename", "")
            filedata_b64 = request.form.get("filedata", "")

            if not target_ip:
                data = request.get_json(silent=True) or {}
                target_ip = data.get("target_ip", "")
                filename = data.get("filename", filename)
                filedata_b64 = data.get("filedata", filedata_b64)

            if not target_ip or not filename or not filedata_b64:
                return jsonify(
                    {"ok": False, "error": "target_ip, filename, filedata required"}
                ), 400

            # 解码 Base64 文件数据
            try:
                filedata = base64.b64decode(filedata_b64)
            except Exception:
                return jsonify({"ok": False, "error": "invalid base64"}), 400

            # 写入临时文件
            os.makedirs("/tmp/p2p_send", exist_ok=True)
            send_path = os.path.join("/tmp/p2p_send", filename)
            with open(send_path, "wb") as f:
                f.write(filedata)

            file_id = generate_uuid()
            file_size = get_file_size(send_path)
            total_chunks = max(1, (file_size + CHUNK_SIZE - 1) // CHUNK_SIZE)

            tm = self._transfer_manager
            if tm:
                # 查找目标设备信息
                dev = None
                dm = self._device_manager
                if dm:
                    dev = dm.find_device_by_ip(target_ip)
                if dev is None:
                    dev = DeviceInfo(ip=target_ip, name=target_ip)

                # 注册传输任务
                task = TransferTask(
                    meta=FileMeta(
                        file_id=file_id,
                        filename=filename,
                        file_size=file_size,
                        total_chunks=total_chunks,
                        chunk_size=CHUNK_SIZE,
                        compression="zlib",
                    ),
                    target=dev,
                    state=TransferState.NEGOTIATING,
                    local_file_path=send_path,
                    is_sender=True,
                )
                tm.add_task(task)

                # 后台线程执行文件发送
                def run_transfer():
                    sender = TransferSender()

                    def progress(chunks, bytes_sent):
                        """更新 TransferManager 中的进度"""
                        tm.update_progress(file_id, chunks, bytes_sent, 0)

                    task_obj = tm.get_task(file_id)
                    if task_obj:
                        task_obj.state = TransferState.TRANSFERRING
                    ok = sender.send_file(
                        target_ip, TRANSFER_PORT, send_path,
                        file_id, 4, progress,
                    )
                    tm.mark_complete(file_id, ok)

                t = threading.Thread(target=run_transfer, daemon=True)
                t.start()

            return jsonify({"ok": True, "file_id": file_id})
