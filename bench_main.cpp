#include "common/platform.h"
#include "common/utils.h"
#include "common/types.h"
#include "transfer/transfer_sender.h"
#include "transfer/transfer_receiver.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <atomic>
#include <vector>
#include <sys/stat.h>

int main(int argc, char** argv) {
    uint64_t file_size_mb = 100;
    uint32_t num_threads = 4;
    uint16_t port = 18890;

    if (argc > 1) file_size_mb = std::stoull(argv[1]);
    if (argc > 2) num_threads = std::stoul(argv[2]);
    if (argc > 3) port = std::stoul(argv[3]);

    std::string test_file = "/tmp/p2p_bench_" + std::to_string(file_size_mb) + "mb.bin";

    if (!NetworkUtils::initialize()) {
        std::cerr << "Network init failed" << std::endl;
        return 1;
    }

    // Generate test file using /dev/urandom
    std::cout << "Generating " << file_size_mb << " MB test file..." << std::endl;
    {
        std::ofstream out(test_file, std::ios::binary);
        std::ifstream urandom("/dev/urandom", std::ios::binary);
        std::vector<char> buf(1024 * 1024);
        for (uint64_t i = 0; i < file_size_mb; ++i) {
            urandom.read(buf.data(), buf.size());
            out.write(buf.data(), buf.size());
        }
        out.close();
    }

    std::cout << "File generated (" << file_size_mb << " MB)" << std::endl;

    // Start receiver
    TransferReceiver receiver(port);
    receiver.set_save_directory("/tmp");

    std::atomic<bool> received{false};
    receiver.set_on_receive_complete([&](const std::string&, const std::string& path, bool success) {
        received = true;
        if (success) {
            std::cout << "Receiver: file OK at " << path << std::endl;
        }
    });

    if (!receiver.start()) {
        std::cerr << "Receiver failed to start" << std::endl;
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Run sender
    TransferSender sender;
    std::string file_id = Utils::generate_uuid();

    auto t0 = std::chrono::steady_clock::now();
    bool ok = sender.send_file("127.0.0.1", port, test_file, file_id, num_threads);
    auto t1 = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "Send " << (ok ? "OK" : "FAIL") << " in " << ms << " ms" << std::endl;

    if (ok) {
        auto metrics = sender.get_metrics();
        std::cout << "Throughput: " << metrics.throughput_mbps << " Mbps" << std::endl;
        std::cout << "Per thread:" << std::endl;
        for (size_t i = 0; i < metrics.threads.size(); ++i) {
            auto& t = metrics.threads[i];
            std::cout << "  T" << i << ": conn=" << t.connection_time_us/1000.0 << "ms"
                      << " first_byte=" << t.first_byte_time_us/1000.0 << "ms"
                      << " total=" << t.total_time_us/1000.0 << "us"
                      << " bytes=" << t.bytes_sent << std::endl;
        }
    }

    // Wait for receiver
    for (int i = 0; i < 30 && !received.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    receiver.stop();
    std::cout << "Receiver " << (received.load() ? "confirmed" : "timeout") << std::endl;

    // Cleanup
    std::remove(test_file.c_str());
    NetworkUtils::cleanup();
    return ok ? 0 : 1;
}
