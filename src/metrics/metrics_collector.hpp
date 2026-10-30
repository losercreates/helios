#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <functional>
#include <map>
#include <utility>

namespace helios {

class Backend;

struct BackendMetricInfo {
    std::string endpoint;
    std::string pool;
    std::shared_ptr<Backend> backend;
};

using BackendHealthProvider = std::function<std::vector<BackendMetricInfo>()>;

// Thread-isolated latency histogram for a single worker
class LatencyHistogram {
public:
    explicit LatencyHistogram(std::vector<uint64_t> bucket_bounds = {50, 100, 300});
    ~LatencyHistogram() = default;

    // Non-copyable, non-movable to ensure atomics remain pinned in memory
    LatencyHistogram(const LatencyHistogram&) = delete;
    LatencyHistogram& operator=(const LatencyHistogram&) = delete;
    LatencyHistogram(LatencyHistogram&&) = delete;
    LatencyHistogram& operator=(LatencyHistogram&&) = delete;

    void Observe(uint64_t latency_us) noexcept;

    [[nodiscard]] const std::vector<uint64_t>& GetBucketBounds() const noexcept { return bucket_bounds_; }
    [[nodiscard]] uint64_t GetBucketCount(size_t index) const noexcept;
    [[nodiscard]] uint64_t GetInfBucketCount() const noexcept { return inf_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t GetSum() const noexcept { return sum_us_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t GetTotalCount() const noexcept { return total_count_.load(std::memory_order_relaxed); }

private:
    std::vector<uint64_t> bucket_bounds_;
    std::vector<std::unique_ptr<std::atomic<uint64_t>>> bucket_counts_;
    std::atomic<uint64_t> inf_count_{0};
    std::atomic<uint64_t> sum_us_{0};
    std::atomic<uint64_t> total_count_{0};
};

// Thread-isolated operational metrics for a single worker thread.
// Completely lock-free on hot paths using relaxed atomics.
struct WorkerMetrics {
    uint32_t worker_id{0};

    // 1. Throughput & Connections
    std::atomic<uint64_t> requests_total{0};
    std::atomic<uint64_t> bytes_read_total{0};
    std::atomic<uint64_t> bytes_written_total{0};
    std::atomic<int64_t> active_connections{0};

    // 2. Latency Histogram
    LatencyHistogram latency_histogram;

    // 3. io_uring Kernel Ring Metrics
    std::atomic<uint64_t> iouring_sqe_submitted_total{0};
    std::atomic<uint64_t> iouring_cqe_reaped_total{0};
    std::atomic<uint64_t> iouring_sq_overflow_total{0};

    // 4. Buffer & Resource Backpressure
    std::atomic<double> buffer_pool_utilization{0.0};
    std::atomic<uint64_t> backpressure_pause_events_total{0};

    // 5. Upstream Connection Pool
    std::atomic<uint64_t> backend_pool_hits_total{0};
    std::atomic<uint64_t> backend_pool_misses_total{0};

    // 6. Security & Traffic Control
    std::atomic<uint64_t> acl_rejected_total{0};
    std::atomic<uint64_t> rate_limit_rejected_total{0};
    std::atomic<uint64_t> slowloris_rejected_total{0};

    // 7. Memory & Resource Footprint
    std::atomic<uint64_t> buffer_pool_bytes_allocated{0};
    std::atomic<uint64_t> buffer_pool_bytes_used{0};
    std::atomic<uint64_t> backend_pool_idle_connections{0};

    // 8. TLS / HTTPS Metrics
    std::atomic<uint64_t> tls_handshakes_total{0};
    std::atomic<uint64_t> tls_handshake_errors_total{0};
    std::atomic<int64_t> tls_active_connections{0};

    // 9. WebSocket Metrics
    std::atomic<uint64_t> websocket_upgrades_total{0};
    std::atomic<int64_t> websocket_active_connections{0};

    explicit WorkerMetrics(uint32_t id, std::vector<uint64_t> latency_buckets = {50, 100, 300})
        : worker_id(id), latency_histogram(std::move(latency_buckets)) {}

    // Non-copyable, non-movable
    WorkerMetrics(const WorkerMetrics&) = delete;
    WorkerMetrics& operator=(const WorkerMetrics&) = delete;
    WorkerMetrics(WorkerMetrics&&) = delete;
    WorkerMetrics& operator=(WorkerMetrics&&) = delete;
};

// Collector and Prometheus text format serializer aggregating metrics across all workers
class MetricsCollector {
public:
    MetricsCollector() = default;
    ~MetricsCollector() = default;

    // Register or retrieve worker metrics instance
    std::shared_ptr<WorkerMetrics> GetOrCreateWorkerMetrics(uint32_t worker_id);
    void RegisterWorkerMetrics(std::shared_ptr<WorkerMetrics> metrics);

    // Render RFC-compliant Prometheus exposition plaintext format (version 0.0.4)
    [[nodiscard]] std::string RenderPrometheusText() const;

    // Clear all registered worker metrics
    void Clear() noexcept;

    // Upstream Backend Health & Probe Telemetry
    void SetBackendHealthProvider(BackendHealthProvider provider);
    void RecordBackendHealth(std::string endpoint, std::string pool, int status);
    void RecordHealthCheckProbe(const std::string& endpoint, const std::string& result);
    [[nodiscard]] uint64_t GetHealthCheckProbesTotal(const std::string& endpoint, const std::string& result) const;

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<WorkerMetrics>> workers_;
    BackendHealthProvider backend_health_provider_;
    std::map<std::pair<std::string, std::string>, int> manual_backend_health_;
    mutable std::mutex probe_metrics_mutex_;
    std::map<std::pair<std::string, std::string>, uint64_t> probe_metrics_;
};

} // namespace helios
