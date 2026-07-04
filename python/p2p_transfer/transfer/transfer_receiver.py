"""
文件传输接收端模块

实现多连接并发文件接收，核心特性：

1. InboundTransfer 状态机：跟踪多连接共享的传输状态
2. 每个 Handler 线程独立接收分块并写入 .tmp 文件
3. 使用 pwrite() 实现线程安全的随机写入
4. 当所有分块接收完毕时，关闭文件并 rename(.tmp → 最终文件)
5. 支持 zlib 解压（若 FILE_RANGE 消息中有 compression="zlib"）
6. 兼容旧版单连接 FILE_HEADER 协议

多连接完成判定：
当 received_count == total_chunks 时立即提交文件，
而非等待所有声明的连接到达（有些线程可能因空范围而跳过）。
此设计比 C++ 原版的 finished_connections 判定更鲁棒。
"""

import socket
import threading
import os
import time
import struct
from dataclasses import dataclass, field
from p2p_transfer.common.types import (
    TRANSFER_PORT,
    CHUNK_SIZE,
    MAX_DATA_SIZE,
    MsgType,
    FileMeta,
)
from p2p_transfer.common.platform import set_reuse_addr
from p2p_transfer.common.peer_key import PeerKey
from p2p_transfer.common.protocol import (
    recv_json_any,
    send_json_with_marker,
    recv_chunk_data,
    build_range_ack,
)
from p2p_transfer.common.utils import decompress_data
from p2p_transfer.transfer.chunk import deserialize_chunk, CHUNK_SENTINEL_INDEX
from p2p_transfer.transfer.file_io import FileChunkIO


@dataclass
class InboundTransfer:
    """入站传输状态机

    多连接共享的传输状态，由 TransferReceiver 的静态 s_inbound 字典管理。
    多个 Handler 线程（每个对应一个发送方连接）协调写入同一文件。

    关键字段：
    - total_connections: 发送方声明的总连接数
    - received_bitmap: 按分块索引的已接收标记
    - committed: 防止重复提交（仅提交一次）
    - start_fired: 保证 on_start 回调仅触发一次
    """
    meta: FileMeta = field(default_factory=FileMeta)
    total_connections: int = 0          # 发送方声明的总连接数
    finished_connections: int = 0        # 已完成的连接数
    received_bitmap: list[bool] = field(default_factory=list)  # 分块位图
    received_count: int = 0              # 已接收分块计数
    save_path: str = ""                  # 最终文件路径
    temp_path: str = ""                  # 临时文件路径 (.tmp)
    committed: bool = False              # 是否已提交（防重复）
    start_fired: bool = False            # 是否已触发 on_start 回调
    file_io: FileChunkIO | None = None  # 文件 I/O 对象


class TransferReceiver:
    """多连接文件传输接收器

    主 accept 线程接受 TCP 连接，为每个连接创建 Handler 线程。
    Handler 线程读取 FILE_RANGE/FILE_HEADER 头部后接收分块数据。
    """

    def __init__(
        self,
        port: int = TRANSFER_PORT,
        save_dir: str = "./received_files",
    ):
        """初始化传输接收器

        Args:
            port: 监听端口（默认 8890）
            save_dir: 接收文件保存目录
        """
        self._port = port
        self._save_dir = save_dir
        self._running = False

        # 套接字与线程管理
        self._sock: socket.socket | None = None
        self._accept_thread: threading.Thread | None = None
        self._handler_threads: list[threading.Thread] = []
        self._handler_lock = threading.Lock()

        # 入站传输状态（file_id → InboundTransfer）
        self._lock = threading.Lock()
        self._inbound: dict[str, InboundTransfer] = {}

        # 回调函数
        self._on_start = None        # callback(file_id, filename, file_size, total_chunks)
        self._on_progress = None     # callback(file_id, received_count, total_chunks)
        self._on_complete = None     # callback(file_id, save_path, file_size)

    def set_on_start(self, callback):
        self._on_start = callback

    def set_on_progress(self, callback):
        self._on_progress = callback

    def set_on_complete(self, callback):
        self._on_complete = callback

    def start(self):
        """启动接收器：创建监听套接字并启动 accept 线程"""
        os.makedirs(self._save_dir, exist_ok=True)
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        set_reuse_addr(self._sock)
        self._sock.bind(("0.0.0.0", self._port))
        self._sock.listen(64)
        self._sock.settimeout(0.5)                # 0.5 秒超时的非阻塞 accept
        self._running = True
        self._accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._accept_thread.start()

    def stop(self):
        """停止接收器：关闭套接字并等待所有线程退出"""
        self._running = False
        if self._accept_thread:
            self._accept_thread.join(timeout=2)
        if self._sock:
            try:
                self._sock.close()
            except:
                pass
        with self._handler_lock:
            for t in self._handler_threads:
                t.join(timeout=2)

    def _accept_loop(self):
        """主 accept 循环：接受连接并创建 Handler 线程"""
        while self._running:
            if self._sock is None:
                break
            try:
                client, addr = self._sock.accept()
            except (socket.timeout, BlockingIOError):
                continue
            except:
                if self._running:
                    continue
                break

            t = threading.Thread(
                target=self._handle_receive,
                args=(client, addr[0]),
                daemon=True,
            )
            t.start()
            with self._handler_lock:
                self._handler_threads.append(t)
                # 清理已结束的线程记录
                self._handler_threads = [
                    h for h in self._handler_threads if h.is_alive()
                ]

    def _handle_receive(self, sock: socket.socket, sender_ip: str):
        """Handler 线程入口：读取头部消息并分派到具体的处理函数

        根据消息类型选择：
        - FILE_RANGE → handle_single_range（多连接协议）
        - FILE_HEADER → handle_single_connection（旧版单连接协议）
        """
        try:
            sock.settimeout(30)
            header = recv_json_any(sock, sender_ip)       # 自动判别加密/明文
            if header is None:
                return

            msg_type = header.get("type", "")

            if msg_type == MsgType.FILE_RANGE:
                self._handle_single_range(sock, header, sender_ip)
            elif msg_type == MsgType.FILE_HEADER:
                self._handle_single_connection(sock, header, sender_ip)

        except Exception as e:
            print(f"[TransferReceiver] Error handling connection: {e}")
        finally:
            try:
                sock.close()
            except:
                pass

    def _handle_single_connection(self, sock: socket.socket, header: dict, sender_ip: str):
        """处理旧版单连接传输（FILE_HEADER 协议）

        兼容性保留：接收分块直到总数达标或遇到哨兵标记。
        """
        file_id = header.get("file_id", "")
        filename = header.get("filename", "")
        file_size = header.get("file_size", 0)
        chunk_size = header.get("chunk_size", CHUNK_SIZE)
        total_chunks = header.get("total_chunks", 0)
        compression = header.get("compression", "")

        if not file_id:
            return

        save_path = os.path.join(self._save_dir, f"{file_id}_{filename}")
        temp_path = save_path + ".tmp"

        # 创建并截断临时文件
        with open(temp_path, "wb") as f:
            f.truncate(file_size)

        # 构造 FileChunkIO（读取 .tmp 文件大小）
        file_io = FileChunkIO(save_path, is_sender=False, file_id=file_id)
        file_io.set_total_size(file_size, total_chunks)

        # 循环接收分块
        chunk_count = 0
        while chunk_count < total_chunks:
            raw = recv_chunk_data(sock, sender_ip)
            if raw is None:
                break
            if raw == b"__JSON_SENTINEL__":
                break                                  # 遇到控制消息哨兵

            result = deserialize_chunk(raw)
            if result is None:
                continue
            chunk_index, _, _, data = result
            if chunk_index == CHUNK_SENTINEL_INDEX:
                break

            file_io.write_chunk(chunk_index, data)
            chunk_count += 1

        # 检查是否完整接收
        if file_io.all_chunks_received():
            file_io.commit_received_file()
            if compression == "zlib":
                self._decompress_file(save_path, file_id)
            if self._on_complete:
                self._on_complete(file_id, save_path, file_size)
        else:
            file_io.cancel_receive()

    def _handle_single_range(self, sock: socket.socket, header: dict, sender_ip: str):
        """处理多连接传输的单个连接范围（FILE_RANGE 协议）

        多连接状态管理流程：
        1. 首次接收到 file_id：创建 InboundTransfer + .tmp 文件
        2. 后续连接加入同一 InboundTransfer
        3. 接收本连接的分块并更新共享位图
        4. 发送 RANGE_ACK 确认
        5. 当所有分块接收完毕时（received_count == total_chunks）提交文件

        此判定条件比原 C++ 的 finished_connections 更鲁棒：
        - 若某个发送线程因空范围而跳过，不会导致接收方永久等待
        """
        # 解析头部字段
        file_id = header.get("file_id", "")
        filename = header.get("filename", "")
        file_size = header.get("file_size", 0)
        chunk_size = header.get("chunk_size", CHUNK_SIZE)
        total_chunks = header.get("total_chunks", 0)
        start_chunk = header.get("start_chunk", 0)
        end_chunk = header.get("end_chunk", 0)
        conn_index = header.get("conn_index", 0)
        total_connections = header.get("total_connections", 1)
        compression = header.get("compression", "")

        if not file_id:
            return

        # ── 创建或获取 InboundTransfer ──
        with self._lock:
            if file_id not in self._inbound:
                save_path = os.path.join(self._save_dir, f"{file_id}_{filename}")
                temp_path = save_path + ".tmp"

                meta = FileMeta(
                    file_id=file_id, filename=filename, file_size=file_size,
                    chunk_size=chunk_size, total_chunks=total_chunks,
                    compression=compression,
                )
                inbound = InboundTransfer(
                    meta=meta,
                    total_connections=total_connections,
                    finished_connections=0,
                    received_bitmap=[False] * total_chunks,
                    received_count=0,
                    save_path=save_path,
                    temp_path=temp_path,
                )

                # 创建 .tmp 文件并截断到预期大小
                with open(temp_path, "wb") as f:
                    f.truncate(file_size)

                # 构造 FileChunkIO（必须在 ftruncate 之后）
                inbound.file_io = FileChunkIO(save_path, is_sender=False, file_id=file_id)
                inbound.file_io.set_total_size(file_size, total_chunks)

                self._inbound[file_id] = inbound

            inbound = self._inbound[file_id]

        # ── 仅触发一次 on_start 回调 ──
        if not inbound.start_fired:
            inbound.start_fired = True
            if self._on_start:
                self._on_start(file_id, filename, file_size, total_chunks)

        # ── 接收本连接范围内的分块 ──
        chunk_count = 0
        for chunk_idx in range(start_chunk, end_chunk + 1):
            raw = recv_chunk_data(sock, sender_ip)
            if raw is None:
                break
            if raw == b"__JSON_SENTINEL__":
                break                              # 遇到控制消息（RANGE_DONE）

            result = deserialize_chunk(raw)
            if result is None:
                continue
            chunk_index, _, _, data = result
            if chunk_index == CHUNK_SENTINEL_INDEX:
                break

            # 写入分块数据并更新共享状态
            with self._lock:
                if inbound.file_io and not inbound.committed:
                    inbound.file_io.write_chunk(chunk_index, data)
                    if chunk_index < len(inbound.received_bitmap):
                        if not inbound.received_bitmap[chunk_index]:
                            inbound.received_bitmap[chunk_index] = True
                            inbound.received_count += 1

            chunk_count += 1
            if self._on_progress:
                self._on_progress(file_id, inbound.received_count, total_chunks)

        # ── 发送 RANGE_ACK 确认 ──
        ack = build_range_ack(file_id, conn_index)
        send_json_with_marker(sock, ack, sender_ip)

        # ── 检查是否可以提交 ──
        # 判定条件：所有分块都已接收（received_count == total_chunks）
        # 此条件比原 C++ 的 finished_connections 判定更可靠
        all_done = False
        with self._lock:
            inbound.finished_connections += 1
            if not inbound.committed and inbound.received_count == total_chunks:
                inbound.committed = True
                all_done = True

        if all_done:
            # 提交文件（关闭 fd + rename .tmp → 最终文件）
            with self._lock:
                if inbound.file_io:
                    inbound.file_io.commit_received_file()
                    inbound.file_io = None

            # zlib 解压（若需要）
            if compression == "zlib":
                self._decompress_file(inbound.save_path, file_id)

            # 触发完成回调
            if self._on_complete:
                self._on_complete(file_id, inbound.save_path, file_size)

            # 清理入站状态
            with self._lock:
                self._inbound.pop(file_id, None)

    def _decompress_file(self, save_path: str, file_id: str):
        """解压接收到的 zlib 压缩文件

        读取 → zlib.decompress() → 写回原路径。
        """
        try:
            with open(save_path, "rb") as f:
                compressed = f.read()
            decompressed = decompress_data(compressed)
            with open(save_path, "wb") as f:
                f.write(decompressed)
        except Exception as e:
            print(f"[TransferReceiver] Decompress error: {e}")
