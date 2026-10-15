#pragma once

#include "metrics_collector.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace helios {

class PrometheusServer {
public:
    struct Config {
        std::string host{"127.0.0.1"};
        uint16_t port{9090}; // 0 = bind ephemeral port
        int backlog{128};
    };

    PrometheusServer();
    explicit PrometheusServer(std::shared_ptr<MetricsCollector> collector);
    PrometheusServer(std::shared_ptr<MetricsCollector> collector, Config config);
    ~PrometheusServer();

    // Non-copyable, non-movable to ensure stable network bindings
    PrometheusServer(const PrometheusServer&) = delete;
    PrometheusServer& operator=(const PrometheusServer&) = delete;
    PrometheusServer(PrometheusServer&&) = delete;
    PrometheusServer& operator=(PrometheusServer&&) = delete;

    // Server lifecycle
    bool Start();
    void Stop() noexcept;

    // Status queries
    [[nodiscard]] bool IsRunning() const noexcept { return running_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint16_t GetPort() const noexcept { return listen_port_; }
    [[nodiscard]] std::shared_ptr<MetricsCollector> GetCollector() const noexcept { return collector_; }

private:
    void EventLoop();
    void HandleClient(int client_fd);
    void CleanupLocked() noexcept;

    Config config_;
    std::shared_ptr<MetricsCollector> collector_;

    std::atomic<bool> running_{false};
    uint16_t listen_port_{0};

    std::atomic<int> listen_fd_{-1};
    std::atomic<int> epoll_fd_{-1};
    std::atomic<int> stop_event_fd_{-1};

    mutable std::mutex state_mutex_;
    std::thread worker_thread_;
};

} // namespace helios
