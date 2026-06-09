#!/usr/bin/env python3
import json
import sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

def plot_metrics(metrics_file, output_prefix="transfer"):
    with open(metrics_file) as f:
        data = json.load(f)

    file_size_mb = data.get("file_size", 0) / (1024 * 1024)
    num_threads = data.get("num_threads", 1)
    total_time_ms = data.get("total_time_us", 0) / 1000.0
    throughput_mbps = data.get("throughput_mbps", 0)
    threads = data.get("threads", [])

    # 1. Speed over time
    plt.figure(figsize=(12, 5))
    all_speeds = []
    all_times = []
    max_time = 0
    for t in threads:
        samples = t.get("speed_samples", [])
        if not samples:
            continue
        times = [s[0] / 1e6 for s in samples]
        speeds = [s[1] / 1e6 for s in samples]
        all_times.append(times)
        all_speeds.append(speeds)
        max_time = max(max_time, times[-1] if times else 0)

    for i, (times, speeds) in enumerate(zip(all_times, all_speeds)):
        label = f"Thread {i} [chunk {threads[i].get('start_chunk', '?')}-{threads[i].get('end_chunk', '?')})"
        plt.step(times, speeds, where='post', label=label, linewidth=1.5)

    plt.xlabel("Time (s)")
    plt.ylabel("Speed (MB/s)")
    plt.title(f"Transfer Speed — {file_size_mb:.1f} MB, {num_threads} threads, {total_time_ms:.0f} ms, {throughput_mbps:.1f} Mbps")
    plt.legend(fontsize=8)
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{output_prefix}_speed.png", dpi=150)
    plt.close()

    # 2. Thread latency breakdown
    plt.figure(figsize=(10, 5))
    labels = []
    conn_times = []
    first_byte_times = []
    total_times = []
    for i, t in enumerate(threads):
        labels.append(f"T{i}")
        conn_times.append(t.get("connection_time_us", 0) / 1000.0)
        first_byte_times.append(t.get("first_byte_time_us", 0) / 1000.0)
        total_times.append(t.get("total_time_us", 0) / 1000.0)

    x = np.arange(len(labels))
    width = 0.25
    plt.bar(x - width, conn_times, width, label='Connect (ms)', color='#4caf50')
    plt.bar(x, [fb - ct for fb, ct in zip(first_byte_times, conn_times)], width, label='First Byte (ms)', color='#2196f3')
    plt.bar(x + width, [tt - fb for tt, fb in zip(total_times, first_byte_times)], width, label='Data Transfer (ms)', color='#ff9800')

    plt.xlabel("Thread")
    plt.ylabel("Time (ms)")
    plt.xticks(x, labels)
    plt.title("Per-Thread Latency Breakdown")
    plt.legend()
    plt.grid(True, alpha=0.3, axis='y')
    plt.tight_layout()
    plt.savefig(f"{output_prefix}_latency.png", dpi=150)
    plt.close()

    # 3. Throughput vs threads
    plt.figure(figsize=(8, 5))
    plt.bar(["Throughput"], [throughput_mbps], color='#673ab7', width=0.4)
    plt.axhline(y=1000, color='r', linestyle='--', alpha=0.5, label='1 Gbps')
    plt.ylabel("Mbps")
    plt.title(f"Effective Throughput — {num_threads} threads, {file_size_mb:.1f} MB")
    plt.legend()
    plt.grid(True, alpha=0.3, axis='y')
    plt.tight_layout()
    plt.savefig(f"{output_prefix}_throughput.png", dpi=150)
    plt.close()

    print(f"Generated: {output_prefix}_speed.png, {output_prefix}_latency.png, {output_prefix}_throughput.png")
    print(f"Summary: {file_size_mb:.1f} MB in {total_time_ms:.0f} ms = {throughput_mbps:.1f} Mbps ({throughput_mbps/1000:.2f} Gbps)")
    for i, t in enumerate(threads):
        print(f"  T{i}: conn={t.get('connection_time_us',0)/1000:.1f}ms first_byte={t.get('first_byte_time_us',0)/1000:.1f}ms total={t.get('total_time_us',0)/1000:.0f}us bytes={t.get('bytes_sent',0)}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: plot_metrics.py <metrics.json> [output_prefix]")
        sys.exit(1)
    prefix = sys.argv[2] if len(sys.argv) > 2 else "transfer"
    plot_metrics(sys.argv[1], prefix)
