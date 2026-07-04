"""
文件传输发送端模块

实现多线程并发文件发送，核心特性：

1. 分块范围划分：将 total_chunks 均匀分配到 N 个线程
2. 每个线程独立打开 TCP 连接，互不干扰
3. 可选 zlib 压缩：整个文件压缩后若更小则使用压缩版本
4. 启用 BBR 拥塞控制（Linux 内核 4.9+，静默回退）
5. 传输指标收集（连接耗时、首字节时间、速度采样）

与 C++ 实现的差异：
- 使用 Python threading 替代 C++ std::thread
- 不会为空白范围（empty range）创建线程，避免超发 total_connections
- 指标输出到 /tmp/p2p_metrics.json
"""

import socket
import threading
import time
import os
import math
import json
from p2p_transfer.common.types import (
    TRANSFER_PORT,
    CHUNK_SIZE,
    NUM_THREADS,
    MsgType,
    TransferState,
    FileMeta,
)
from p2p_transfer.common.platform import enable_bbr
from p2p_transfer.common.peer_key import PeerKey
from p2p_transfer.common.protocol import (
    send_json_with_marker,
    recv_json_any,
    send_chunk_data,
    build_file_range,
    build_range_done,
)
from p2p_transfer.common.utils import compress_data, get_filename
from p2p_transfer.transfer.chunk import serialize_chunk
from p2p_transfer.transfer.file_io import FileChunkIO


class ThreadMetrics:
    """单个发送线程的性能指标收集器

    记录连接时间、首字节时间、发送字节数/块数、速度采样点。
    """

    def __init__(self):
        self.connection_time_us = 0         # TCP 连接耗时（微秒）
        self.first_byte_time_us = 0         # 首字节发送时间（微秒自连接开始）
        self.total_time_us = 0              # 总耗时（微秒）
        self.bytes_sent = 0                 # 已发送字节数
        self.chunks_sent = 0                # 已发送分块数
        self.speed_samples: list[tuple[float, float]] = []  # (时间戳, 累积字节)


class TransferSender:
    """多线程文件传输发送器

    使用方法：
        sender = TransferSender()
        sender.send_file(target_ip, port, file_path, file_id, num_threads=4)
    """

    def __init__(self):
        self._num_threads = NUM_THREADS

    def send_file(
        self,
        target_ip: str,
        target_port: int,
        file_path: str,
        file_id: str,
        num_threads: int = NUM_THREADS,
        progress_callback=None,
    ) -> bool:
        """向目标设备发送文件

        流程：
        1. 读取源文件并尝试 zlib 压缩
        2. 若压缩后更小则使用压缩文件，否则使用原文件
        3. 计算分块数，按线程数分配范围
        4. 过滤空白范围，计算实际连接数
        5. 为每个有效范围创建一个发送线程
        6. 等待所有线程完成
        7. 写出指标文件并清理临时压缩文件

        Args:
            target_ip: 目标设备 IP
            target_port: 目标传输端口（通常 8890）
            file_path: 源文件绝对路径
            file_id: 传输任务的 UUID
            num_threads: 并行线程数
            progress_callback: 进度回调 callback(chunks_done, bytes_sent)

        Returns:
            所有线程是否成功完成
        """
        self._num_threads = num_threads

        file_name = get_filename(file_path)
        original_size = os.path.getsize(file_path)
        total_chunks = max(1, math.ceil(original_size / CHUNK_SIZE))
        compressed = False
        actual_path = file_path
        actual_size = original_size

        try:
            # ── 压缩决策 ──
            with open(file_path, "rb") as f:
                file_data = f.read()
            compressed_data = compress_data(file_data)
            if len(compressed_data) < len(file_data):
                # 压缩后更小，写入临时压缩文件并使用
                compressed = True
                compressed_path = f"/tmp/p2p_send_compressed_{file_id}"
                actual_path = compressed_path
                actual_size = len(compressed_data)
                total_chunks = max(1, math.ceil(actual_size / CHUNK_SIZE))
                with open(compressed_path, "wb") as f:
                    f.write(compressed_data)

            # ── 范围划分与线程创建 ──
            # 计算原始分配范围
            chunk_ranges = self._divide_chunks(total_chunks, self._num_threads)
            # 过滤掉 end < start 的空白范围，避免发送无效连接
            active_ranges = [(i, s, e) for i, (s, e) in enumerate(chunk_ranges) if s <= e]
            actual_connections = len(active_ranges)
            if actual_connections == 0:
                return False

            threads = []
            metrics_list = [ThreadMetrics() for _ in range(actual_connections)]
            errors = []                               # 线程错误收集

            for conn_i, (orig_i, start, end) in enumerate(active_ranges):
                t = threading.Thread(
                    target=self._send_chunk_range,
                    args=(
                        target_ip, target_port, actual_path, file_id,
                        file_name, actual_size, total_chunks,
                        start, end, conn_i, actual_connections,
                        compressed, metrics_list[conn_i], errors,
                    ),
                    daemon=True,
                )
                t.start()
                threads.append(t)

            # ── 进度报告（如果提供了回调） ──
            if progress_callback:
                while any(t.is_alive() for t in threads):
                    total_bytes = sum(m.bytes_sent for m in metrics_list)
                    total_chunks_done = sum(m.chunks_sent for m in metrics_list)
                    progress_callback(total_chunks_done, total_bytes)
                    time.sleep(0.2)

            # 等待所有线程完成
            for t in threads:
                t.join(timeout=30)

            # 写出指标文件
            self._write_metrics(metrics_list, target_ip, file_name, actual_size)

            # 清理临时压缩文件
            if compressed and os.path.exists(actual_path) and actual_path != file_path:
                try:
                    os.unlink(actual_path)
                except:
                    pass

            return len(errors) == 0

        except Exception as e:
            print(f"[TransferSender] Error: {e}")
            return False

    def _divide_chunks(self, total_chunks: int, n_threads: int) -> list[tuple[int, int]]:
        """将总分块数均匀分配到 n_threads 个线程

        使用公平分配算法：
        - 基础分配 = total_chunks // n_threads
        - 余数分配给前 rem 个线程
        - 若某个线程分配数为 0，设范围为 (0, -1) 表示空

        Returns:
            列表，每个元素为 (start_chunk, end_chunk)
        """
        ranges = []
        base = total_chunks // n_threads          # 每个线程基础块数
        rem = total_chunks % n_threads             # 剩余块数（分配给前几个线程）
        start = 0
        for i in range(n_threads):
            extra = 1 if i < rem else 0
            count = base + extra
            if count == 0:
                ranges.append((0, -1))             # 空范围：标记为无效
            else:
                end = start + count - 1
                ranges.append((start, end))
                start = end + 1
        return ranges

    def _send_chunk_range(
        self,
        target_ip: str,
        target_port: int,
        file_path: str,
        file_id: str,
        file_name: str,
        file_size: int,
        total_chunks: int,
        start_chunk: int,
        end_chunk: int,
        conn_index: int,
        total_connections: int,
        compressed: bool,
        metrics: ThreadMetrics,
        errors: list,
    ):
        """单个线程的发送逻辑

        独立 TCP 连接，发送指定范围的分块，
        完成后等待 RANGE_ACK 确认。

        有线协议顺序：
        ┌─────────────┬──────────────┬────────────┬────────────┬──────────┐
        │ FILE_RANGE  │ chunk_0..N-1 │ RANGE_DONE │ wait       │ close    │
        │ (J/E JSON)  │ (C/D binary) │ (J/E JSON) │ RANGE_ACK  │          │
        └─────────────┴──────────────┴────────────┴────────────┴──────────┘
        """
        # 防御性检查：空范围跳过
        if start_chunk > end_chunk or end_chunk < 0:
            return

        sock = None
        connect_start = time.time()
        try:
            # ── 连接与配置 ──
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 256 * 1024)  # 256KB 发送缓冲
            sock.settimeout(10)

            sock.connect((target_ip, target_port))
            metrics.connection_time_us = int((time.time() - connect_start) * 1_000_000)

            # 尝试启用 BBR 拥塞控制
            enable_bbr(sock)

            # ── 发送 FILE_RANGE 头部 ──
            comp_str = "zlib" if compressed else ""
            header = build_file_range(
                file_id, file_name, file_size, total_chunks, CHUNK_SIZE,
                start_chunk, end_chunk, conn_index, total_connections, comp_str,
            )
            send_json_with_marker(sock, header, target_ip)

            # ── 打开文件并发送分块 ──
            file_io = FileChunkIO(file_path, is_sender=True, file_id=file_id)

            sock.settimeout(5)
            first_chunk = True

            for chunk_idx in range(start_chunk, end_chunk + 1):
                data = file_io.read_chunk(chunk_idx)
                if data is None:
                    break
                # 序列化为有线格式
                chunk_bytes = serialize_chunk(chunk_idx, total_chunks, file_id, data)
                send_chunk_data(sock, chunk_bytes, target_ip)        # 自动加密

                # 记录首字节时间
                if first_chunk:
                    metrics.first_byte_time_us = int(
                        (time.time() - connect_start) * 1_000_000
                    )
                    first_chunk = False

                metrics.bytes_sent += len(data)
                metrics.chunks_sent += 1

                # 速度采样（每 100ms 一个采样点）
                now = time.time()
                if metrics.speed_samples:
                    last_t, last_b = metrics.speed_samples[-1]
                    if now - last_t >= 0.1:
                        metrics.speed_samples.append((now, metrics.bytes_sent))
                else:
                    metrics.speed_samples.append((now, 0))

            file_io.close()

            # ── 发送 RANGE_DONE 并等待 RANGE_ACK ──
            done_msg = build_range_done(file_id, conn_index)
            send_json_with_marker(sock, done_msg, target_ip)

            sock.settimeout(10)
            ack = recv_json_any(sock, target_ip)
            if ack is None or ack.get("type") != MsgType.RANGE_ACK:
                raise Exception(f"No valid RANGE_ACK received, got: {ack}")

        except Exception as e:
            errors.append(str(e))
        finally:
            if sock:
                try:
                    sock.close()
                except:
                    pass

    def _write_metrics(
        self, metrics_list: list[ThreadMetrics], target_ip: str, file_name: str, file_size: int
    ):
        """将各线程的传输指标汇总写入 /tmp/p2p_metrics.json"""
        summary = {
            "target": target_ip,
            "file": file_name,
            "size": file_size,
            "threads": [],
        }
        total_bytes = 0
        for mi, m in enumerate(metrics_list):
            tinfo = {
                "index": mi,
                "bytes": m.bytes_sent,
                "chunks": m.chunks_sent,
                "connect_us": m.connection_time_us,     # 连接耗时（微秒）
                "first_byte_us": m.first_byte_time_us,   # 首字节时间（微秒）
            }
            total_bytes += m.bytes_sent
            summary["threads"].append(tinfo)

        summary["total_bytes"] = total_bytes
        try:
            with open("/tmp/p2p_metrics.json", "w") as f:
                json.dump(summary, f, indent=2)
        except:
            pass
