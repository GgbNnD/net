#!/usr/bin/env python3
import pexpect, sys, json, time, os, subprocess

REMOTE = "l@10.162.181.2"
PASS = "1"
REMOTE_PORT = 18890

def ssh_cmd(cmd, timeout=120):
    child = pexpect.spawn(f'ssh -o StrictHostKeyChecking=no {REMOTE} "{cmd}"',
                          timeout=timeout, encoding='utf-8')
    try:
        idx = child.expect(['password:', 'yes/no', pexpect.EOF, pexpect.TIMEOUT], timeout=10)
        if idx == 0:
            child.sendline(PASS)
        elif idx == 1:
            child.sendline('yes')
            child.expect('password:', timeout=5)
            child.sendline(PASS)
        child.expect(pexpect.EOF, timeout=timeout)
    except Exception as e:
        child.close()
        return f"ERROR: {e}"
    output = child.before or ""
    child.close()
    return output.strip()

def ssh_bg(cmd, wait=2):
    child = pexpect.spawn(f'ssh -o StrictHostKeyChecking=no {REMOTE} "{cmd}"',
                          timeout=300, encoding='utf-8')
    try:
        idx = child.expect(['password:', 'yes/no', pexpect.EOF, pexpect.TIMEOUT], timeout=10)
        if idx == 0:
            child.sendline(PASS)
        elif idx == 1:
            child.sendline('yes')
            child.expect('password:', timeout=5)
            child.sendline(PASS)
        child.expect(pexpect.EOF, timeout=5)
    except pexpect.TIMEOUT:
        pass
    except:
        pass
    time.sleep(wait)
    return child

all_results = {}

# ========== Phase 0: Baseline ==========
print("=== Baseline ===")
result = subprocess.run(["ping", "-c", "10", "-q", "10.162.181.2"], capture_output=True, text=True, timeout=15)
for line in result.stdout.split("\n"):
    if "avg" in line or "rtt" in line:
        print(f"  {line.strip()}")
        parts = line.split("/")
        if len(parts) >= 5:
            all_results["rtt_avg_ms"] = float(parts[4])

# ========== Phase 1: Build remote ==========
print("\n=== Build Remote ===")
ssh_cmd("pkill -9 P2PBench 2>/dev/null; pkill -9 P2PFileTransfer 2>/dev/null; sleep 1; echo cleaned", timeout=5)
out = ssh_cmd("cd /home/cells/net && cmake -B build -S . 2>&1 | tail -2", timeout=30)
print(f"  CMake: {out}")
out = ssh_cmd("cd /home/cells/net && cmake --build build -j$(nproc) --target P2PBench 2>&1 | tail -3", timeout=120)
print(f"  Build: {out}")

# Kill local services
subprocess.run(["pkill", "-9", "P2PFileTransfer"], capture_output=True)
subprocess.run(["pkill", "-9", "P2PBench"], capture_output=True)
time.sleep(2)

# ========== Phase 2: Start services ==========
print("\n=== Start Services ===")
# Remote: start P2PFileTransfer (for signaling)
ssh_bg("nohup /home/cells/net/build/bin/P2PFileTransfer > /tmp/p2p_remote_srv.log 2>&1 &")
time.sleep(3)
out = ssh_cmd("ss -tlnp | grep -E '8889|8890|8891'", timeout=5)
print(f"  Remote services: {out[:200]}")

# Local: start P2PFileTransfer
local_srv = subprocess.Popen(
    ["./build/bin/P2PFileTransfer"],
    stdout=open("/tmp/p2p_local_srv.log", "w"), stderr=subprocess.STDOUT
)
time.sleep(3)
result = subprocess.run(["curl", "-s", "-X", "POST", "http://localhost:8891/api/peers/add",
                         "-d", "ip=10.162.181.2"], capture_output=True, text=True, timeout=5)
print(f"  Local add peer: {result.stdout}")

out = ssh_cmd("curl -s -X POST http://localhost:8891/api/peers/add -d 'ip=10.162.44.132'", timeout=5)
print(f"  Remote add peer: {out}")

print("  Waiting for mutual discovery (15s)...")
time.sleep(15)

result = subprocess.run(["curl", "-s", "http://localhost:8891/api/devices"], capture_output=True, text=True, timeout=5)
print(f"  Local devices: {result.stdout[:300]}")

# ========== Phase 3: ECDH + Signaling ==========
print("\n=== Signaling ===")
# Check ECDH logs
with open("/tmp/p2p_local_srv.log") as f:
    for line in f:
        if "密钥" in line or "ECDH" in line or "hello" in line.lower():
            print(f"  Local: {line.strip()}")
out = ssh_cmd("grep -E '密钥|ECDH' /tmp/p2p_remote_srv.log 2>/dev/null", timeout=5)
print(f"  Remote ECDH: {out[:200]}")

# TEXT_MESSAGE RTT
t0 = time.time()
result = subprocess.run(["curl", "-s", "-X", "POST", "http://localhost:8891/api/message",
                         "-d", "target_ip=10.162.181.2&text=benchmark+test"],
                       capture_output=True, text=True, timeout=10)
t1 = time.time()
msg_rtt = (t1 - t0) * 1000
print(f"  TEXT_MESSAGE RTT: {msg_rtt:.1f} ms, response={result.stdout}")

# Send message reverse direction
out = ssh_cmd("curl -s -X POST http://localhost:8891/api/message -d 'target_ip=10.162.44.132&text=hello_back'", timeout=10)
print(f"  Reverse message: {out}")

# ========== Phase 4: Start bench receiver on remote ==========
print("\n=== Start Bench Receiver ===")
ssh_cmd(f"pkill -9 P2PBench 2>/dev/null; sleep 1; echo done", timeout=5)
ssh_bg(f"nohup /home/cells/net/build/bin/P2PBench --recv {REMOTE_PORT} > /tmp/p2p_recv.log 2>&1 &")
time.sleep(2)
out = ssh_cmd(f"ss -tlnp | grep {REMOTE_PORT}", timeout=5)
print(f"  Remote bench receiver: {out}")

# ========== Phase 5: File Transfer Tests ==========
print("\n=== File Transfers ===")
all_results["transfers"] = []

for file_mb in [1, 100, 500, 1000]:
    for threads in [1, 4]:
        label = f"{file_mb}MB_t{threads}"
        print(f"\n--- {label} ---")

        # Generate test file locally
        gen_result = subprocess.run(
            ["./build/bin/P2PBench", "--gen", str(file_mb)],
            capture_output=True, text=True, timeout=300
        )
        test_file = f"/tmp/p2p_bench_{file_mb}mb.bin"
        print(f"  Generated: {test_file}")

        # Send from local -> remote
        print(f"  Sending {file_mb}MB -> remote ({threads} threads)...")
        t0 = time.time()
        result = subprocess.run(
            ["./build/bin/P2PBench", "--send", "10.162.181.2", str(file_mb), str(threads), str(REMOTE_PORT)],
            capture_output=True, text=True, timeout=600
        )
        t1 = time.time()
        elapsed = t1 - t0
        out = result.stdout + result.stderr

        rd = {"file_mb": file_mb, "threads": threads, "direction": "local->remote",
              "elapsed_sec": elapsed, "ok": "SEND OK" in out}
        for line in out.split("\n"):
            line = line.strip()
            if line.startswith("THROUGHPUT_MBPS="):
                rd["throughput_mbps"] = float(line.split("=")[1])
            elif line.startswith("TOTAL_TIME_US="):
                rd["total_time_us"] = int(line.split("=")[1])
            elif line.startswith("FILE_SIZE="):
                rd["file_size"] = int(line.split("=")[1])
            elif line.startswith("THREAD_"):
                parts = line.split()
                ti = int(parts[0].split("_")[1])
                td = {}
                for p in parts[1:]:
                    if "=" in p:
                        k, v = p.split("=")
                        td[k] = float(v) if "." in v else int(v)
                rd.setdefault("threads_detail", {})[ti] = td

        status = "OK" if rd.get("ok") else "FAIL"
        tp = rd.get("throughput_mbps", 0)
        print(f"  L->R: {status}, {tp:.1f} Mbps, {elapsed:.1f}s")

        # Also check received file on remote
        out2 = ssh_cmd(f"ls -la /tmp/p2p_bench_{file_mb}mb.bin", timeout=5)
        rd["received_on_remote"] = "p2p_bench" in out2

        all_results["transfers"].append(rd)
        subprocess.run(["rm", "-f", test_file], capture_output=True)
        ssh_cmd(f"rm -f /tmp/p2p_bench_{file_mb}mb.bin", timeout=5)
        time.sleep(2)

# ========== Save ==========
with open("/tmp/p2p_test_results.json", "w") as f:
    json.dump(all_results, f, indent=2)
print(f"\n=== Results: /tmp/p2p_test_results.json ===")
print(json.dumps(all_results, indent=2))

# ========== Cleanup ==========
local_srv.terminate()
ssh_cmd("pkill -9 P2PBench 2>/dev/null; pkill -9 P2PFileTransfer 2>/dev/null; echo done", timeout=5)
print("\nDone")
