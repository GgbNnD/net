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
#include <cstring>
#include <sys/stat.h>

static void generate_file(const std::string& path, uint64_t size_mb) {
    std::ofstream out(path, std::ios::binary);
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    std::vector<char> buf(1024 * 1024);
    for (uint64_t i = 0; i < size_mb; ++i) {
        urandom.read(buf.data(), buf.size());
        out.write(buf.data(), buf.size());
    }
    out.close();
}

int main(int argc, char** argv) {
    if (!NetworkUtils::initialize()) {
        std::cerr << "Network init failed" << std::endl;
        return 1;
    }

    if (argc >= 2 && std::strcmp(argv[1], "--recv") == 0) {
        uint16_t port = (argc >= 3) ? std::stoul(argv[2]) : 18890;
        std::cout << "RECEIVER mode on port " << port << std::endl;

        TransferReceiver receiver(port);
        receiver.set_save_directory("/tmp");
        receiver.set_on_receive_complete([](const std::string& fid, const std::string& path, bool ok) {
            std::cout << "RECV_DONE: " << fid << " -> " << path << " " << (ok ? "OK" : "FAIL") << std::endl;
        });
        receiver.set_on_receive_start([](const std::string& fid, const std::string& name,
                                          uint64_t size, uint32_t chunks, const std::string& ip) {
            std::cout << "RECV_START: " << name << " " << size << " bytes " << chunks << " chunks from " << ip << std::endl;
        });

        if (!receiver.start()) { std::cerr << "Receiver start failed" << std::endl; return 1; }
        std::cout << "Receiver ready. Press Ctrl+C to stop." << std::endl;

        while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
        return 0;
    }

    if (argc >= 2 && std::strcmp(argv[1], "--gen") == 0) {
        uint64_t size_mb = (argc >= 3) ? std::stoull(argv[2]) : 100;
        std::string path = "/tmp/p2p_bench_" + std::to_string(size_mb) + "mb.bin";
        std::cout << "Generating " << size_mb << " MB..." << std::endl;
        generate_file(path, size_mb);
        std::cout << "Generated: " << path << std::endl;
        return 0;
    }

    uint64_t file_size_mb = 100;
    uint32_t num_threads = 4;
    uint16_t port = 18890;
    std::string target_ip = "127.0.0.1";

    if (argc >= 2 && std::strcmp(argv[1], "--send") == 0) {
        if (argc < 3) { std::cerr << "Usage: --send <ip> [size_mb] [threads] [port]" << std::endl; return 1; }
        target_ip = argv[2];
        if (argc >= 4) file_size_mb = std::stoull(argv[3]);
        if (argc >= 5) num_threads = std::stoul(argv[4]);
        if (argc >= 6) port = std::stoul(argv[5]);
    } else {
        if (argc >= 2) file_size_mb = std::stoull(argv[1]);
        if (argc >= 3) num_threads = std::stoul(argv[2]);
        if (argc >= 4) port = std::stoul(argv[3]);
    }

    std::string test_file = "/tmp/p2p_bench_" + std::to_string(file_size_mb) + "mb.bin";

    bool is_local = (target_ip == "127.0.0.1" || target_ip == "localhost");

    // Generate file
    if (!is_local || argc < 2 || std::strcmp(argv[1], "--send") != 0) {
        struct stat st;
        if (stat(test_file.c_str(), &st) != 0 || st.st_size == 0) {
            std::cout << "Generating " << file_size_mb << " MB test file..." << std::endl;
            generate_file(test_file, file_size_mb);
        }
    } else {
        struct stat st;
        if (stat(test_file.c_str(), &st) != 0 || st.st_size == 0) {
            generate_file(test_file, file_size_mb);
        }
    }

    std::cout << "File ready: " << test_file << " (" << file_size_mb << " MB)" << std::endl;

    std::unique_ptr<TransferReceiver> receiver;
    if (is_local) {
        receiver = std::make_unique<TransferReceiver>(port);
        receiver->set_save_directory("/tmp");
        receiver->set_on_receive_complete([](const std::string&, const std::string& path, bool ok) {
            std::cout << "Receiver: " << path << " " << (ok ? "OK" : "FAIL") << std::endl;
        });
        if (!receiver->start()) { std::cerr << "Receiver start failed" << std::endl; return 1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    TransferSender sender;
    std::string file_id = Utils::generate_uuid();

    auto t0 = std::chrono::steady_clock::now();
    bool ok = sender.send_file(target_ip, port, test_file, file_id, num_threads);
    auto t1 = std::chrono::steady_clock::now();

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "SEND " << (ok ? "OK" : "FAIL") << " total_ms=" << ms << std::endl;

    if (ok) {
        auto m = sender.get_metrics();
        std::cout << "THROUGHPUT_MBPS=" << m.throughput_mbps << std::endl;
        std::cout << "FILE_SIZE=" << m.file_size << std::endl;
        std::cout << "NUM_THREADS=" << m.num_threads << std::endl;
        std::cout << "TOTAL_TIME_US=" << m.total_time_us << std::endl;
        for (size_t i = 0; i < m.threads.size(); ++i) {
            auto& t = m.threads[i];
            std::cout << "THREAD_" << i
                      << " conn_us=" << t.connection_time_us
                      << " first_byte_us=" << t.first_byte_time_us
                      << " total_us=" << t.total_time_us
                      << " bytes=" << t.bytes_sent
                      << " start_chunk=" << t.start_chunk
                      << " end_chunk=" << t.end_chunk
                      << std::endl;
        }
    }

    if (is_local) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        receiver->stop();
    }

    std::remove(test_file.c_str());
    NetworkUtils::cleanup();
    return ok ? 0 : 1;
}
