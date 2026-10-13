#include "foundation.hpp"
#include "worker.hpp"
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <string_view>

namespace {
std::atomic<bool> g_stop_signal{false};

void SignalHandler(int signum) {
    (void)signum;
    g_stop_signal.store(true, std::memory_order_relaxed);
}
} // namespace

int main(int argc, char* argv[]) {
    auto info = helios::GetSystemInfo();

    std::cout << "[HELIOS] Starting " << info.name << " v" << info.version << "\n";
    std::cout << "[HELIOS] Compiled with C++ standard: " << info.cpp_standard << "\n";

    // Setup signal handling for graceful shutdown
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    bool daemon_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--daemon" || std::string_view(argv[i]) == "-d") {
            daemon_mode = true;
        }
    }

    helios::WorkerPoolConfig pool_config;
    pool_config.num_workers = std::thread::hardware_concurrency();
    if (pool_config.num_workers == 0) pool_config.num_workers = 2;
    pool_config.listen_port = 8080;
    pool_config.drain_timeout = std::chrono::milliseconds(15000);

    helios::WorkerPool pool(pool_config);
    pool.AddBackend("127.0.0.1", 9001);

    if (pool.Start()) {
        std::cout << "[HELIOS] Multi-Worker Thread-per-Core Proxy initialized with "
                  << pool.GetWorkerCount() << " workers on port " << pool.GetPort() << "\n";

        if (daemon_mode) {
            std::cout << "[HELIOS] Running in daemon mode. Send SIGINT/SIGTERM to stop.\n";
            while (!g_stop_signal.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } else {
            // Self-validation run (e.g. for startup integration tests)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        std::cout << "[HELIOS] Initiating graceful draining on all workers...\n";
        pool.Stop();
    } else {
        std::cout << "[HELIOS] Multi-worker listener initialization failed on port " << pool_config.listen_port << "\n";
    }

    std::cout << "[HELIOS] Exiting cleanly (0).\n";
    return 0;
}
