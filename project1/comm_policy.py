import random

import matplotlib.pyplot as plt
import numpy as np

NUM_NODES = 20
SIMULATION_TIME = 5000
MAX_BACKOFF = 10
PACKET_LENGTH = 5
NUM_TRIALS = 20


def _generate_packets(nodes_queue, arrival_rate):
    packets_generated = 0
    for i in range(NUM_NODES):
        if random.random() < arrival_rate:
            nodes_queue[i] += 1
            packets_generated += 1
    return packets_generated


def simulate_csmacd(arrival_rate):
    """Simulate slotted CSMA/CD with binary exponential backoff."""
    nodes_queue = np.zeros(NUM_NODES)
    nodes_backoff = np.zeros(NUM_NODES)
    nodes_collisions = np.zeros(NUM_NODES)

    successful_transmissions = 0
    total_delay = 0
    packets_generated = 0
    channel_busy_until = 0

    for t in range(SIMULATION_TIME):
        packets_generated += _generate_packets(nodes_queue, arrival_rate)

        total_delay += np.sum(nodes_queue)

        if t >= channel_busy_until:
            ready_nodes = [
                i for i in range(NUM_NODES) if nodes_queue[i] > 0 and nodes_backoff[i] <= 0
            ]

            if len(ready_nodes) == 1:
                sender = ready_nodes[0]
                successful_transmissions += PACKET_LENGTH
                nodes_queue[sender] -= 1
                nodes_collisions[sender] = 0
                channel_busy_until = t + PACKET_LENGTH
            elif len(ready_nodes) > 1:
                for i in ready_nodes:
                    nodes_collisions[i] = min(nodes_collisions[i] + 1, MAX_BACKOFF)
                    max_slots = (2 ** int(nodes_collisions[i])) - 1
                    nodes_backoff[i] = random.randint(1, max_slots + 1)
                channel_busy_until = t + 1

        for i in range(NUM_NODES):
            if nodes_backoff[i] > 0:
                nodes_backoff[i] -= 1

    throughput = successful_transmissions / SIMULATION_TIME
    avg_delay = total_delay / max(1, packets_generated)
    return throughput, avg_delay


def simulate_csmadcr(arrival_rate):
    """Simulate CSMA/DCR: collision then deterministic ID-ordered service."""
    nodes_queue = np.zeros(NUM_NODES)

    successful_transmissions = 0
    total_delay = 0
    packets_generated = 0

    resolution_queue = []
    channel_busy_until = 0

    for t in range(SIMULATION_TIME):
        packets_generated += _generate_packets(nodes_queue, arrival_rate)

        total_delay += np.sum(nodes_queue)

        if t < channel_busy_until:
            continue

        if resolution_queue:
            sender = resolution_queue.pop(0)
            if nodes_queue[sender] > 0:
                successful_transmissions += PACKET_LENGTH
                nodes_queue[sender] -= 1
                channel_busy_until = t + PACKET_LENGTH
            continue

        ready_nodes = [i for i in range(NUM_NODES) if nodes_queue[i] > 0]

        if len(ready_nodes) == 1:
            sender = ready_nodes[0]
            successful_transmissions += PACKET_LENGTH
            nodes_queue[sender] -= 1
            channel_busy_until = t + PACKET_LENGTH
        elif len(ready_nodes) > 1:
            resolution_queue.extend(sorted(ready_nodes))
            channel_busy_until = t + 1

    throughput = successful_transmissions / SIMULATION_TIME
    avg_delay = total_delay / max(1, packets_generated)
    return throughput, avg_delay


def run_trials(sim_fn, arrival_rate, trials=NUM_TRIALS):
    throughputs = []
    delays = []
    for _ in range(trials):
        t, d = sim_fn(arrival_rate)
        throughputs.append(t)
        delays.append(d)
    return float(np.mean(throughputs)), float(np.mean(delays))


arrival_rates = np.linspace(0.001, 0.015, 20)
throughput_cd = []
delay_cd = []
throughput_dcr = []
delay_dcr = []

print("Running simulation...")
for rate in arrival_rates:
    t_cd, d_cd = run_trials(simulate_csmacd, rate)
    t_dcr, d_dcr = run_trials(simulate_csmadcr, rate)

    throughput_cd.append(t_cd)
    delay_cd.append(d_cd)
    throughput_dcr.append(t_dcr)
    delay_dcr.append(d_dcr)

plt.figure(figsize=(12, 5))
plt.subplot(1, 2, 1)
plt.plot(arrival_rates * NUM_NODES, throughput_cd, label="CSMA/CD (Traditional)", marker="o")
plt.plot(arrival_rates * NUM_NODES, throughput_dcr, label="CSMA/DCR (Proposed)", marker="s")
plt.title("Throughput vs. Network Load")
plt.xlabel("Total Network Load (Packets / Slot)")
plt.ylabel("Channel Utilization")
plt.grid(True)
plt.legend()

plt.subplot(1, 2, 2)
plt.plot(arrival_rates * NUM_NODES, delay_cd, label="CSMA/CD (Traditional)", marker="o")
plt.plot(arrival_rates * NUM_NODES, delay_dcr, label="CSMA/DCR (Proposed)", marker="s")
plt.title("Average Delay vs. Network Load")
plt.xlabel("Total Network Load (Packets / Slot)")
plt.ylabel("Average Delay (Slots)")
plt.yscale("log")
plt.grid(True)
plt.legend()
plt.tight_layout()
plt.show()
