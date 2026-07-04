"""
TCP 信令服务端模块

短连接模式的 TCP 信令服务器，端口 8889。
每个连接处理一条请求消息，发送应答后立即关闭。

处理的消息类型：
- DEVICE_HELLO：设备探测握手，回送 DEVICE_HELLO_ACK（含 ECDH 公钥）
- FILE_REQUEST：文件传输请求，通过回调交由上层处理
- TEXT_MESSAGE：文本消息，回送 TEXT_ACK
- TRANSFER_CANCEL/PAUSE/RESUME/DONE/ERROR：控制消息，回送 CONTROL_ACK

线程管理：
- 主 accept 线程使用 select/settimeout 模式，非阻塞等待连接
- 每个客户端连接创建一个 handler 线程处理
- 所有 handler 线程在 stop() 时被 join（不 detach）
"""

import socket
import threading
from p2p_transfer.common.types import SIGNALING_PORT, MsgType
from p2p_transfer.common.platform import set_reuse_addr
from p2p_transfer.common.protocol import (
    recv_json_message,
    send_json_message,
    build_device_hello_ack,
)


class SignalingServer:
    """短连接 TCP 信令服务器

    接收消息 → 回调通知上层 → 发送应答 → 关闭连接。
    支持 ECDH 公钥交换，DEVICE_HELLO 应答中携带服务器公钥。
    """

    def __init__(
        self,
        port: int = SIGNALING_PORT,
        ecdh_public_key: str = "",
    ):
        """初始化信令服务器

        Args:
            port: 监听端口（默认 8889）
            ecdh_public_key: 本机 ECDH 公钥（Base64），在 DEVICE_HELLO_ACK 中发送
        """
        self._port = port
        self._running = False

        # 套接字与线程管理
        self._sock: socket.socket | None = None
        self._accept_thread: threading.Thread | None = None
        self._handler_threads: list[threading.Thread] = []
        self._handler_lock = threading.Lock()

        self._ecdh_public_key = ecdh_public_key

        # 消息回调
        self._file_request_cb = None
        self._device_hello_cb = None
        self._control_msg_cb = None
        self._text_msg_cb = None

    # ── 回调设置 ──

    def set_file_request_callback(self, callback):
        """FILE_REQUEST 回调：callback(msg, sender_ip, sock)"""
        self._file_request_cb = callback

    def set_device_hello_callback(self, callback):
        """DEVICE_HELLO 回调：callback(msg, sender_ip)"""
        self._device_hello_cb = callback

    def set_control_msg_callback(self, callback):
        """控制消息回调（TRANSFER_CANCEL 等）：callback(msg, sender_ip)"""
        self._control_msg_cb = callback

    def set_text_msg_callback(self, callback):
        """TEXT_MESSAGE 回调：callback(msg, sender_ip)"""
        self._text_msg_cb = callback

    def set_ecdh_public_key(self, key: str):
        """设置 ECDH 公钥（可在启动后更新）"""
        self._ecdh_public_key = key

    # ── 生命周期 ──

    def start(self):
        """启动服务器：创建监听套接字并启动 accept 线程"""
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        set_reuse_addr(self._sock)
        self._sock.bind(("0.0.0.0", self._port))
        self._sock.listen(64)
        # 使用 0.2 秒超时的 accept，允许定期检查 _running 标志
        self._sock.settimeout(0.2)
        self._running = True
        self._accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._accept_thread.start()

    def stop(self):
        """停止服务器：关闭套接字并等待所有线程退出"""
        self._running = False
        if self._accept_thread:
            self._accept_thread.join(timeout=2)
        if self._sock:
            try:
                self._sock.close()
            except:
                pass
        # 等待所有 handler 线程退出
        with self._handler_lock:
            for t in self._handler_threads:
                t.join(timeout=2)

    # ── accept 循环 ──

    def _accept_loop(self):
        """主 accept 循环：接受连接并创建 handler 线程

        使用 settimeout(0.2) 的非阻塞式 accept，
        每次 accept 后清理已完成的 handler 线程记录。
        """
        while self._running:
            if self._sock is None:
                break
            try:
                client, addr = self._sock.accept()
            except (socket.timeout, BlockingIOError):
                continue                # 超时检查 _running
            except:
                if self._running:
                    continue
                break

            # 为每个连接创建独立的处理线程
            t = threading.Thread(
                target=self._handle_client,
                args=(client, addr[0]),
                daemon=True,
            )
            t.start()
            # 追踪 handler 线程，同时清理已结束的线程
            with self._handler_lock:
                self._handler_threads.append(t)
                self._handler_threads = [
                    h for h in self._handler_threads if h.is_alive()
                ]

    # ── 客户端处理 ──

    def _handle_client(self, sock: socket.socket, client_ip: str):
        """处理单个客户端连接：接收一条消息 → 分发 → 应答 → 关闭

        使用 recv_json_message 的智能模式，自动判别明文/加密消息。
        所有消息类型（除 FILE_REQUEST 外）均发送 ACK 应答。
        """
        try:
            sock.settimeout(5)                       # 5 秒接收超时
            msg = recv_json_message(sock, client_ip)  # 智能接收（自动解密）
            if msg is None:
                return

            msg_type = msg.get("type", "")

            if msg_type == MsgType.DEVICE_HELLO:
                # 设备探测握手：通知上层 → 回送携带本机公钥的 ACK
                if self._device_hello_cb:
                    self._device_hello_cb(msg, client_ip)

                ack = build_device_hello_ack(self._ecdh_public_key)
                send_json_message(sock, ack, client_ip)

            elif msg_type == MsgType.FILE_REQUEST:
                # 文件请求：交由上层回调处理（包括发送应答）
                if self._file_request_cb:
                    self._file_request_cb(msg, client_ip, sock)

            elif msg_type == MsgType.TEXT_MESSAGE:
                # 文本消息：通知上层 → 回送 TEXT_ACK
                if self._text_msg_cb:
                    self._text_msg_cb(msg, client_ip)

                ack = {"type": MsgType.TEXT_ACK}
                send_json_message(sock, ack, client_ip)

            elif msg_type in (
                MsgType.TRANSFER_CANCEL,
                MsgType.TRANSFER_PAUSE,
                MsgType.TRANSFER_RESUME,
                MsgType.TRANSFER_DONE,
                MsgType.TRANSFER_ERROR,
            ):
                # 传输控制消息：通知上层 → 回送 CONTROL_ACK
                if self._control_msg_cb:
                    self._control_msg_cb(msg, client_ip)

                ack = {"type": MsgType.CONTROL_ACK}
                send_json_message(sock, ack, client_ip)

        except Exception as e:
            print(f"[SignalingServer] Handler error: {e}")
        finally:
            # 短连接模式：处理完毕后立即关闭
            try:
                sock.close()
            except:
                pass
