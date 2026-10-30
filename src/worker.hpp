#pragma once

#include "net/l4_proxy_server.hpp"
#include "tls/tls_context.hpp"
#include "routing/l7_route.hpp"
#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <tuple>
#include <chrono>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace helios {

class HealthCheckManager;

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
    bool enable_connection_pooling{false};
    BackendConnectionPoolConfig connection_pool_config{};
    std::chrono::milliseconds header_read_timeout{5000};
    SecurityConfig security{};
    TlsConfig tls{};
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
    void AddBackend(std::shared_ptr<Backend> backend) { proxy_.AddBackend(std::move(backend)); }
    void AddBackendToNamedPool(const std::string& pool_name, std::shared_ptr<Backend> backend) {
        proxy_.AddBackendToNamedPool(pool_name, std::move(backend));
    }
    void SetListenPort(uint16_t port) noexcept;
    void Post(std::function<void()> task);
    void SetAccessLogger(std::shared_ptr<AccessLogger> logger) noexcept { proxy_.SetAccessLogger(std::move(logger)); }
    void SetMetrics(std::shared_ptr<WorkerMetrics> metrics) noexcept { proxy_.SetMetrics(std::move(metrics)); }

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

    std::mutex task_mutex_;
    std::vector<std::function<void()>> pending_tasks_;
    std::atomic<bool> has_tasks_{false};

    std::thread thread_;
};

// Orchestrates a pool of Worker instances pinned across CPU cores.
class WorkerPool {
public:
    struct BackendInfo {
        std::string host;
        uint16_t port{80};
        uint32_t weight{1};
    };

    explicit WorkerPool(WorkerPoolConfig config);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    void AddBackend(std::string host, uint16_t port, uint32_t weight = 1);
    void AddBackend(std::shared_ptr<Backend> backend);
    void ReloadBackends(const std::vector<BackendInfo>& new_backends);
    void SetNamedPoolsAndRoutes(const std::vector<BackendPoolConfig>& pools,
                                const std::vector<L7RouteConfig>& routes,
                                const std::string& default_pool);
    void ReloadNamedPoolsAndRoutes(const std::vector<BackendPoolConfig>& pools,
                                   const std::vector<L7RouteConfig>& routes,
                                   const std::string& default_pool);
    [[nodiscard]] std::vector<std::shared_ptr<Backend>> GetAllBackends() const;

    void SetHealthCheckManager(std::shared_ptr<HealthCheckManager> manager);
    [[nodiscard]] std::shared_ptr<HealthCheckManager> GetHealthCheckManager() const noexcept {
        return health_check_manager_;
    }
    void PurgeBackendConnections(uint32_t backend_id);
    [[nodiscard]] std::vector<BackendMetricInfo> GetBackendMetricInfos() const;
    void SetAccessLogger(std::shared_ptr<AccessLogger> logger);
    [[nodiscard]] std::shared_ptr<AccessLogger> GetAccessLogger() const noexcept { return access_logger_; }
    void SetMetricsCollector(std::shared_ptr<MetricsCollector> collector);
    [[nodiscard]] std::shared_ptr<MetricsCollector> GetMetricsCollector() const noexcept { return metrics_collector_; }
    void SetAcl(std::shared_ptr<IpAcl> acl);
    [[nodiscard]] std::shared_ptr<IpAcl> GetAcl() const noexcept { return acl_; }
    void SetRateLimiter(std::shared_ptr<RateLimiter> limiter);
    [[nodiscard]] std::shared_ptr<RateLimiter> GetRateLimiter() const noexcept { return rate_limiter_; }
    void SetTlsContext(std::shared_ptr<TlsContext> ctx);
    [[nodiscard]] std::shared_ptr<TlsContext> GetTlsContext() const noexcept { return tls_context_; }
    void ReloadSecurity(const SecurityConfig& security);
    bool ReloadTls(const TlsConfig& tls_config, std::string* error_out = nullptr);

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
    std::shared_ptr<AccessLogger> access_logger_;
    std::shared_ptr<MetricsCollector> metrics_collector_;
    std::shared_ptr<IpAcl> acl_;
    std::shared_ptr<RateLimiter> rate_limiter_;
    std::shared_ptr<TlsContext> tls_context_;
    std::shared_ptr<HealthCheckManager> health_check_manager_;
    std::vector<BackendPoolConfig> initial_pools_;
    std::vector<L7RouteConfig> initial_routes_;
    std::string initial_default_pool_;
    mutable std::mutex backend_cache_mutex_;
    std::unordered_map<std::string, std::shared_ptr<Backend>> backend_cache_;
    uint32_t next_backend_id_{1};
    std::atomic<bool> running_{false};
};

} // namespace helios
