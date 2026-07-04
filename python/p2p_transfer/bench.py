#!/usr/bin/env python3
"""
P2P 文件传输基准测试工具

支持四种运行模式：
1. 默认模式：本地回环基准测试（接收端 + 发送端同时启动）
2. --recv PORT：仅启动接收端（用于跨机器测试）
3. --send IP MB THREADS PORT：仅启动发送端（用于跨机器测试）
4. --gen MB：仅生成测试文件

使用方式：
    python bench.py                  # 本地基准测试（10MB, 4 线程）
    python bench.py 20 8             # 20MB, 8 线程
    python bench.py 50 4 19990       # 指定端口
    python bench.py --recv 8888      # 接收端模式
    python bench.py --send 10.0.0.1 100 8 8890  # 发送端模式
    python bench.py --gen 100        # 生成 100MB 测试文件

测试文件：/tmp/p2p_bench_{size}mb.bin（从 /dev/urandom 生成）
指标输出：/tmp/p2p_metrics.json"""

import os
import sys
import time
import argparse

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from p2p_transfer.common.utils import generate_uuid
from p2p_transfer.transfer.transfer_receiver import TransferReceiver
from p2p_transfer.transfer.transfer_sender import TransferSender

# 默认参数
DEFAULT_PORT = 18890          # 默认测试端口（避免与生产端口冲突）
DEFAULT_SIZE_MB = 10          # 默认传输大小
DEFAULT_THREADS = 4           # 默认并行线程数


def gen_test_file(size_mb: int) -> str:
    """生成指定大小的随机测试文件

    若文件已存在且大小相同则直接复用，否则重新生成。
    数据来自 /dev/urandom（非压缩数据），确保测试结果反映真实传输性能。

    Returns:
        测试文件的绝对路径
    """
    path = f"/tmp/p2p_bench_{size_mb}mb.bin"
    if os.path.exists(path):
        existing = os.path.getsize(path)
        if existing == size_mb * 1024 * 1024:
            return path
    print(f"Generating {size_mb}MB test file...")
    with open("/dev/urandom", "rb") as rng:
        data = rng.read(size_mb * 1024 * 1024)
    with open(path, "wb") as f:
        f.write(data)
    print(f"  Created {path} ({os.path.getsize(path)} bytes)")
    return path


def run_receiver_only(port: int):
    """仅接收端模式：持续监听并报告每次接收的性能"""
    recv_dir = "/tmp/p2p_bench_recv"
    os.makedirs(recv_dir, exist_ok=True)
    print(f"Receiver listening on port {port}, saving to {recv_dir}")
    print("Press Ctrl+C to stop.")

    start_time = time.time()

    def on_complete(fid, path, size):
        """每次接收完成时输出性能指标"""
        elapsed = time.time() - start_time
        mb = size / (1024 * 1024)
        speed = mb / elapsed if elapsed > 0 else 0
        print(
            f"[RECV] {os.path.basename(path)}: "
            f"{mb:.1f}MB in {elapsed:.2f}s ({speed:.1f} MB/s)"
        )

    receiver = TransferReceiver(port=port, save_dir=recv_dir)
    receiver.set_on_complete(on_complete)
    receiver.start()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass

    receiver.stop()


def run_sender_only(ip: str, size_mb: int, threads: int, port: int):
    """仅发送端模式：生成测试文件并发送到指定目标"""
    path = gen_test_file(size_mb)
    file_id = generate_uuid()

    print(f"Sending {size_mb}MB to {ip}:{port} with {threads} threads...")
    print(f"  File: {path}")

    sender = TransferSender()
    start = time.time()

    def progress(chunks, bytes_sent):
        """实时进度回调：百分比、已传输量、速率"""
        elapsed = time.time() - start
        mb = bytes_sent / (1024 * 1024)
        speed = mb / elapsed if elapsed > 0 else 0
        total_mb = size_mb
        pct = (bytes_sent * 100) / (total_mb * 1024 * 1024) if total_mb > 0 else 0
        print(f"\r  {pct:.0f}%  {mb:.1f}MB  {speed:.1f}MB/s", end="", flush=True)

    ok = sender.send_file(ip, port, path, file_id, threads, progress)
    elapsed = time.time() - start

    print()
    if ok:
        mb = size_mb
        speed = mb / elapsed if elapsed > 0 else 0
        print(f"Transfer complete: {mb}MB in {elapsed:.2f}s ({speed:.1f} MB/s)")
    else:
        print("Transfer FAILED")


def run_local_bench(size_mb: int, threads: int, port: int):
    """本地回环基准测试：在同进程中启动接收端和发送端

    流程：
    1. 生成测试文件
    2. 启动 TransferReceiver（后台线程）
    3. 调用 TransferSender 发送文件
    4. 计算并输出性能指标
    5. 清理临时接收目录
    """
    import threading

    print(f"Local benchmark: {size_mb}MB, {threads} threads, port {port}")
    path = gen_test_file(size_mb)

    recv_dir = "/tmp/p2p_bench_recv"
    os.makedirs(recv_dir, exist_ok=True)

    complete = threading.Event()

    def on_complete(fid, path, size):
        complete.set()

    receiver = TransferReceiver(port=port, save_dir=recv_dir)
    receiver.set_on_complete(on_complete)
    receiver.start()
    time.sleep(0.3)                           # 等待接收端就绪

    sender = TransferSender()
    file_id = generate_uuid()
    start = time.time()

    ok = sender.send_file("127.0.0.1", port, path, file_id, threads)
    elapsed = time.time() - start

    receiver.stop()

    if ok:
        mb = size_mb
        speed = mb / elapsed if elapsed > 0 else 0
        print(f"Transfer complete: {mb}MB in {elapsed:.2f}s ({speed:.1f} MB/s)")
    else:
        print("Transfer FAILED")

    import shutil
    shutil.rmtree(recv_dir, ignore_errors=True)


def main():
    """命令行入口"""
    parser = argparse.ArgumentParser(description="P2P File Transfer Benchmark")
    parser.add_argument("--recv", type=int, metavar="PORT",
                        help="Receiver-only mode (listen and receive indefinitely)")
    parser.add_argument("--send", nargs=4, metavar=("IP", "MB", "THREADS", "PORT"),
                        help="Sender-only mode (generate and send)")
    parser.add_argument("--gen", type=int, metavar="MB",
                        help="Only generate a test file of given size")
    parser.add_argument("size_mb", nargs="?", type=int, default=DEFAULT_SIZE_MB,
                        help=f"File size in MB (default: {DEFAULT_SIZE_MB})")
    parser.add_argument("threads", nargs="?", type=int, default=DEFAULT_THREADS,
                        help=f"Thread count (default: {DEFAULT_THREADS})")
    parser.add_argument("port", nargs="?", type=int, default=DEFAULT_PORT,
                        help=f"Port number (default: {DEFAULT_PORT})")

    args = parser.parse_args()

    if args.gen:
        gen_test_file(args.gen)
        return

    if args.recv:
        run_receiver_only(args.recv)
        return

    if args.send:
        ip, mb, threads, port = args.send
        run_sender_only(ip, int(mb), int(threads), int(port))
        return

    run_local_bench(args.size_mb, args.threads, args.port)


if __name__ == "__main__":
    main()
