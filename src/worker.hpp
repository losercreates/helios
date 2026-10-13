#pragma once

#include "net/l4_proxy_server.hpp"
#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <tuple>
#include <chrono>

namespace helios {

struct WorkerConfig {
    uint32_t worker_id{0};
    int core_id{0};
    L4ProxyServer::Config proxy_config;
    std::chrono::milliseconds drain_timeout{15000};
};

struct WorkerPoolConfig {
    size_t num_workers{0}; // 0 = std::thread::hardware_concurrency()
    std::string listen_host{"127.0.0.1"};
    uint16_t listen_port{8080};
    std::string lb_algorithm{"round_robin"};
    size_t ring_entries{512};
    size_t total_buffers{1024};
    size_t buffer_size{16384};
    std::chrono::milliseconds drain_timeout{15000};
};

// Represents an isolated, thread-per-core Worker running on a dedicated CPU core.
// Shared-nothing: owns private RingEngine, BufferPool, connection table, and listener socket.
class Worker {
public:
    Worker(uint32_t worker_id, int core_id, WorkerConfig config);
    ~Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&) = delete;
    Worker& operator=(Worker&&) = delete;

    bool Start();
    void Stop();
    void NotifyStop() noexcept;
    void Join();

    void AddBackend(std::string host, uint16_t port, uint32_t weight = 1);
    void SetListenPort(uint16_t port) noexcept;

    [[nodiscard]] uint32_t GetWorkerId() const noexcept { return worker_id_; }
    [[nodiscard]] int GetCoreId() const noexcept { return core_id_; }
    [[nodiscard]] bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] bool IsListenerClosed() const noexcept { return listener_closed_.load(std::memory_order_acquire); }
    [[nodiscard]] uint16_t GetPort() const noexcept { return proxy_.GetPort(); }
    [[nodiscard]] size_t GetActiveConnections() const noexcept { return proxy_.GetActiveConnectionCount(); }
    [[nodiscard]] uint64_t GetAcceptedConnections() const noexcept { return proxy_.GetTotalAcceptedConnections(); }
    [[nodiscard]] L4ProxyServer& GetProxy() noexcept { return proxy_; }
    [[nodiscard]] int GetEventFd() const noexcept { return event_fd_; }
    [[nodiscard]] bool GetAffinityStatus() const noexcept { return affinity_set_.load(std::memory_order_acquire); }

    // Test verification: validates thread affinity mask against expected core
    [[nodiscard]] bool CheckAffinity(int expected_core) const;

private:
    void Run();
    bool PinToCore(int core_id);

    uint32_t worker_id_;
    int core_id_;
    WorkerConfig config_;
    L4ProxyServer proxy_;
    int event_fd_{-1};

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> affinity_set_{false};
    std::atomic<bool> listener_closed_{false};
    std::thread thread_;
};

// Orchestrates a pool of Worker instances pinned across CPU cores.
class WorkerPool {
public:
    explicit WorkerPool(WorkerPoolConfig config);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void AddBackend(std::string host, uint16_t port, uint32_t weight = 1);

    bool Start();
    void Stop();
    void NotifyStop() noexcept;
    void Join();

    [[nodiscard]] size_t GetWorkerCount() const noexcept { return workers_.size(); }
    [[nodiscard]] Worker& GetWorker(size_t index) { return *workers_.at(index); }
    [[nodiscard]] const Worker& GetWorker(size_t index) const { return *workers_.at(index); }
    [[nodiscard]] uint16_t GetPort() const noexcept { return actual_port_; }
    [[nodiscard]] size_t GetTotalActiveConnections() const noexcept;
    [[nodiscard]] uint64_t GetTotalAcceptedConnections() const noexcept;
    [[nodiscard]] bool AreAllListenersClosed() const noexcept;
    [[nodiscard]] bool IsRunning() const noexcept;

private:
    WorkerPoolConfig config_;
    uint16_t actual_port_{0};
    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<std::tuple<std::string, uint16_t, uint32_t>> initial_backends_;
    std::atomic<bool> running_{false};
};

} // namespace helios
