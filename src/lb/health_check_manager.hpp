#pragma once

#include "io/ring_engine.hpp"
#include "lb/backend.hpp"
#include "lb/health_checker.hpp"
#include "config/config_loader.hpp"
#include <memory>
#include <vector>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

namespace helios {

// Dedicated daemon-level active health check orchestrator.
// Owns an isolated io_uring RingEngine and dedicated event loop thread,
// periodically probing upstream backend endpoints and atomically updating their health state.
class HealthCheckManager {
public:
    explicit HealthCheckManager(HealthCheckConfig config);
    ~HealthCheckManager();

    HealthCheckManager(const HealthCheckManager&) = delete;
    HealthCheckManager& operator=(const HealthCheckManager&) = delete;
    HealthCheckManager(HealthCheckManager&&) = delete;
    HealthCheckManager& operator=(HealthCheckManager&&) = delete;

    bool Start();
    void Stop();

    void SetConfig(HealthCheckConfig config);
    [[nodiscard]] const HealthCheckConfig& GetConfig() const noexcept { return config_; }

    void RegisterBackend(std::shared_ptr<Backend> backend);
    void UnregisterBackend(uint32_t backend_id);
    void UpdateBackends(const std::vector<std::shared_ptr<Backend>>& backends);

    [[nodiscard]] bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] size_t GetActiveCheckerCount() const;

private:
    void Run();
    void PostTask(std::function<void()> task);

    HealthCheckConfig config_;
    RingEngine engine_;
    int event_fd_{-1};

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    mutable std::mutex checkers_mutex_;
    std::unordered_map<uint32_t, std::unique_ptr<HealthChecker>> checkers_;
    std::vector<std::shared_ptr<Backend>> registered_backends_;

    std::mutex task_mutex_;
    std::vector<std::function<void()>> pending_tasks_;
    std::atomic<bool> has_tasks_{false};

    std::thread thread_;
};

} // namespace helios

