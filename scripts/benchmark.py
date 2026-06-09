#!/usr/bin/env python3
import subprocess, os, time, json, base64, sys

FILE_SIZE_MB = int(sys.argv[1]) if len(sys.argv) > 1 else 100
NUM_THREADS = int(sys.argv[2]) if len(sys.argv) > 2 else 4
SERVER_PORT = 18891
TRANSFER_PORT = 18892

test_file = f"/tmp/p2p_bench_{FILE_SIZE_MB}mb.bin"

print(f"Generating {FILE_SIZE_MB} MB test file...")
subprocess.run(["dd", f"if=/dev/urandom", f"of={test_file}", f"bs=1M", f"count={FILE_SIZE_MB}"],
               stderr=subprocess.DEVNULL, check=True)

print("Starting receiver on port", TRANSFER_PORT)
receiver_log = open("/tmp/p2p_receiver.log", "w")
receiver = subprocess.Popen(
    ["./build/bin/P2PFileTransfer"],
    stdout=receiver_log, stderr=receiver_log,
    env={**os.environ, "P2P_BENCH_PORT": str(TRANSFER_PORT)}
)
time.sleep(2)

print("Starting sender...")
with open(test_file, "rb") as f:
    filedata = base64.b64encode(f.read()).decode()

body = f"target_ip=127.0.0.1&target_port={TRANSFER_PORT}&filename=bench_{FILE_SIZE_MB}mb.bin&filedata={filedata}"
result = subprocess.run(
    ["curl", "-s", "-X", "POST", f"http://localhost:{SERVER_PORT}/api/transfer", "-d", body],
    capture_output=True, text=True, timeout=120
)
print("HTTP response:", result.stdout)

time.sleep(5)

if os.path.exists("/tmp/p2p_metrics.json"):
    with open("/tmp/p2p_metrics.json") as f:
        metrics = json.load(f)
    print(json.dumps(metrics, indent=2))
    
    subprocess.run([sys.executable, "scripts/plot_metrics.py", "/tmp/p2p_metrics.json", "transfer_bench"],
                   check=True)
    print("Charts generated: transfer_bench_speed.png, transfer_bench_latency.png, transfer_bench_throughput.png")
else:
    print("No metrics file found")

receiver.terminate()
receiver.wait(timeout=5)
os.remove(test_file)
print("Done")
