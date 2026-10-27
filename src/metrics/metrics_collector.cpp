#include "metrics_collector.hpp"
#include "metrics/system_memory.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace helios {

// ============================================================================
// LatencyHistogram Implementation
// ============================================================================

LatencyHistogram::LatencyHistogram(std::vector<uint64_t> bucket_bounds)
    : bucket_bounds_(std::move(bucket_bounds)) {
    std::sort(bucket_bounds_.begin(), bucket_bounds_.end());
    bucket_counts_.reserve(bucket_bounds_.size());
    for (size_t i = 0; i < bucket_bounds_.size(); ++i) {
        bucket_counts_.push_back(std::make_unique<std::atomic<uint64_t>>(0));
    }
}

void LatencyHistogram::Observe(uint64_t latency_us) noexcept {
    sum_us_.fetch_add(latency_us, std::memory_order_relaxed);
    total_count_.fetch_add(1, std::memory_order_relaxed);
    inf_count_.fetch_add(1, std::memory_order_relaxed);

    for (size_t i = 0; i < bucket_bounds_.size(); ++i) {
        if (latency_us <= bucket_bounds_[i]) {
            bucket_counts_[i]->fetch_add(1, std::memory_order_relaxed);
        }
    }
}

uint64_t LatencyHistogram::GetBucketCount(size_t index) const noexcept {
    if (index < bucket_counts_.size()) {
        return bucket_counts_[index]->load(std::memory_order_relaxed);
    }
    return 0;
}

// ============================================================================
// MetricsCollector Implementation
// ============================================================================

std::shared_ptr<WorkerMetrics> MetricsCollector::GetOrCreateWorkerMetrics(uint32_t worker_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& w : workers_) {
        if (w->worker_id == worker_id) {
            return w;
        }
    }
    auto w = std::make_shared<WorkerMetrics>(worker_id);
    workers_.push_back(w);
    return w;
}

void MetricsCollector::RegisterWorkerMetrics(std::shared_ptr<WorkerMetrics> metrics) {
    if (!metrics) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& w : workers_) {
        if (w->worker_id == metrics->worker_id) {
            return; // Already registered
        }
    }
    workers_.push_back(std::move(metrics));
}

void MetricsCollector::Clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    workers_.clear();
}

std::string MetricsCollector::RenderPrometheusText() const {
    std::vector<std::shared_ptr<WorkerMetrics>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = workers_;
    }

    std::sort(snapshot.begin(), snapshot.end(), [](const auto& a, const auto& b) {
        return a->worker_id < b->worker_id;
    });

    std::ostringstream ss;

    // 1. helios_requests_total
    ss << "# HELP helios_requests_total Total number of HTTP requests processed\n";
    ss << "# TYPE helios_requests_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_requests_total{worker=\"" << w->worker_id << "\", protocol=\"http1\"} "
           << w->requests_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 2. helios_bytes_read_total
    ss << "# HELP helios_bytes_read_total Total bytes read from client sockets\n";
    ss << "# TYPE helios_bytes_read_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_bytes_read_total{worker=\"" << w->worker_id << "\"} "
           << w->bytes_read_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 3. helios_bytes_written_total
    ss << "# HELP helios_bytes_written_total Total bytes written to client sockets\n";
    ss << "# TYPE helios_bytes_written_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_bytes_written_total{worker=\"" << w->worker_id << "\"} "
           << w->bytes_written_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 4. helios_active_connections
    ss << "# HELP helios_active_connections Current active client connections\n";
    ss << "# TYPE helios_active_connections gauge\n";
    for (const auto& w : snapshot) {
        ss << "helios_active_connections{worker=\"" << w->worker_id << "\"} "
           << w->active_connections.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 5. helios_added_latency_microseconds
    ss << "# HELP helios_added_latency_microseconds Added proxy latency in microseconds\n";
    ss << "# TYPE helios_added_latency_microseconds histogram\n";
    for (const auto& w : snapshot) {
        const auto& hist = w->latency_histogram;
        const auto& bounds = hist.GetBucketBounds();
        for (size_t i = 0; i < bounds.size(); ++i) {
            ss << "helios_added_latency_microseconds_bucket{worker=\"" << w->worker_id
               << "\", le=\"" << bounds[i] << "\"} " << hist.GetBucketCount(i) << "\n";
        }
        ss << "helios_added_latency_microseconds_bucket{worker=\"" << w->worker_id
           << "\", le=\"+Inf\"} " << hist.GetInfBucketCount() << "\n";
        ss << "helios_added_latency_microseconds_sum{worker=\"" << w->worker_id << "\"} "
           << hist.GetSum() << "\n";
        ss << "helios_added_latency_microseconds_count{worker=\"" << w->worker_id << "\"} "
           << hist.GetTotalCount() << "\n";
    }
    ss << "\n";

    // 6. helios_iouring_sqe_submitted_total
    ss << "# HELP helios_iouring_sqe_submitted_total Total io_uring SQEs submitted\n";
    ss << "# TYPE helios_iouring_sqe_submitted_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_iouring_sqe_submitted_total{worker=\"" << w->worker_id << "\"} "
           << w->iouring_sqe_submitted_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 7. helios_iouring_cqe_reaped_total
    ss << "# HELP helios_iouring_cqe_reaped_total Total io_uring CQEs reaped\n";
    ss << "# TYPE helios_iouring_cqe_reaped_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_iouring_cqe_reaped_total{worker=\"" << w->worker_id << "\"} "
           << w->iouring_cqe_reaped_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 8. helios_iouring_sq_overflow_total
    ss << "# HELP helios_iouring_sq_overflow_total io_uring kernel SQ ring overflow count\n";
    ss << "# TYPE helios_iouring_sq_overflow_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_iouring_sq_overflow_total{worker=\"" << w->worker_id << "\"} "
           << w->iouring_sq_overflow_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 9. helios_buffer_pool_utilization
    ss << "# HELP helios_buffer_pool_utilization Buffer pool utilization ratio (0.0 - 1.0)\n";
    ss << "# TYPE helios_buffer_pool_utilization gauge\n";
    for (const auto& w : snapshot) {
        ss << std::fixed << std::setprecision(2);
        ss << "helios_buffer_pool_utilization{worker=\"" << w->worker_id << "\"} "
           << w->buffer_pool_utilization.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 10. helios_backpressure_pause_events_total
    ss << "# HELP helios_backpressure_pause_events_total Number of times read/accept SQEs paused due to backpressure\n";
    ss << "# TYPE helios_backpressure_pause_events_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_backpressure_pause_events_total{worker=\"" << w->worker_id << "\"} "
           << w->backpressure_pause_events_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 11. helios_backend_pool_hits_total
    ss << "# HELP helios_backend_pool_hits_total Total pooled backend connections reused\n";
    ss << "# TYPE helios_backend_pool_hits_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_backend_pool_hits_total{worker=\"" << w->worker_id << "\"} "
           << w->backend_pool_hits_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 12. helios_backend_pool_misses_total
    ss << "# HELP helios_backend_pool_misses_total Total backend connections newly created (pool miss)\n";
    ss << "# TYPE helios_backend_pool_misses_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_backend_pool_misses_total{worker=\"" << w->worker_id << "\"} "
           << w->backend_pool_misses_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 13. helios_acl_rejected_total
    ss << "# HELP helios_acl_rejected_total Total connections rejected by IP ACL rules\n";
    ss << "# TYPE helios_acl_rejected_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_acl_rejected_total{worker=\"" << w->worker_id << "\"} "
           << w->acl_rejected_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 14. helios_rate_limit_rejected_total
    ss << "# HELP helios_rate_limit_rejected_total Total requests or connections rejected by rate limiting\n";
    ss << "# TYPE helios_rate_limit_rejected_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_rate_limit_rejected_total{worker=\"" << w->worker_id << "\"} "
           << w->rate_limit_rejected_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 15. helios_slowloris_rejected_total
    ss << "# HELP helios_slowloris_rejected_total Total connections terminated due to slow header read timeout\n";
    ss << "# TYPE helios_slowloris_rejected_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_slowloris_rejected_total{worker=\"" << w->worker_id << "\"} "
           << w->slowloris_rejected_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 16. helios_process_resident_memory_bytes & helios_process_virtual_memory_bytes
    ProcessMemoryInfo mem = QueryProcessMemory();
    ss << "# HELP helios_process_resident_memory_bytes Resident memory size of the Helios process in bytes\n";
    ss << "# TYPE helios_process_resident_memory_bytes gauge\n";
    ss << "helios_process_resident_memory_bytes " << mem.resident_bytes << "\n\n";

    ss << "# HELP helios_process_virtual_memory_bytes Virtual memory size of the Helios process in bytes\n";
    ss << "# TYPE helios_process_virtual_memory_bytes gauge\n";
    ss << "helios_process_virtual_memory_bytes " << mem.virtual_bytes << "\n\n";

    // 17. helios_buffer_pool_bytes_allocated
    ss << "# HELP helios_buffer_pool_bytes_allocated Total bytes allocated for connection buffer pools\n";
    ss << "# TYPE helios_buffer_pool_bytes_allocated gauge\n";
    for (const auto& w : snapshot) {
        ss << "helios_buffer_pool_bytes_allocated{worker=\"" << w->worker_id << "\"} "
           << w->buffer_pool_bytes_allocated.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 18. helios_buffer_pool_bytes_used
    ss << "# HELP helios_buffer_pool_bytes_used Total bytes currently used by active connection buffers\n";
    ss << "# TYPE helios_buffer_pool_bytes_used gauge\n";
    for (const auto& w : snapshot) {
        ss << "helios_buffer_pool_bytes_used{worker=\"" << w->worker_id << "\"} "
           << w->buffer_pool_bytes_used.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 19. helios_connection_pool_idle_connections
    ss << "# HELP helios_connection_pool_idle_connections Total idle backend connections currently pooled\n";
    ss << "# TYPE helios_connection_pool_idle_connections gauge\n";
    for (const auto& w : snapshot) {
        ss << "helios_connection_pool_idle_connections{worker=\"" << w->worker_id << "\"} "
           << w->backend_pool_idle_connections.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 20. helios_tls_handshakes_total
    ss << "# HELP helios_tls_handshakes_total Total successful TLS handshakes completed\n";
    ss << "# TYPE helios_tls_handshakes_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_tls_handshakes_total{worker=\"" << w->worker_id << "\"} "
           << w->tls_handshakes_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 21. helios_tls_handshake_errors_total
    ss << "# HELP helios_tls_handshake_errors_total Total TLS handshake failures or protocol errors\n";
    ss << "# TYPE helios_tls_handshake_errors_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_tls_handshake_errors_total{worker=\"" << w->worker_id << "\"} "
           << w->tls_handshake_errors_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 22. helios_tls_active_connections
    ss << "# HELP helios_tls_active_connections Current active TLS client connections\n";
    ss << "# TYPE helios_tls_active_connections gauge\n";
    for (const auto& w : snapshot) {
        ss << "helios_tls_active_connections{worker=\"" << w->worker_id << "\"} "
           << w->tls_active_connections.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 23. helios_websocket_upgrades_total
    ss << "# HELP helios_websocket_upgrades_total Total successful WebSocket HTTP upgrade handshakes\n";
    ss << "# TYPE helios_websocket_upgrades_total counter\n";
    for (const auto& w : snapshot) {
        ss << "helios_websocket_upgrades_total{worker=\"" << w->worker_id << "\"} "
           << w->websocket_upgrades_total.load(std::memory_order_relaxed) << "\n";
    }
    ss << "\n";

    // 24. helios_websocket_active_connections
    ss << "# HELP helios_websocket_active_connections Current active upgraded WebSocket client connections\n";
    ss << "# TYPE helios_websocket_active_connections gauge\n";
    for (const auto& w : snapshot) {
        ss << "helios_websocket_active_connections{worker=\"" << w->worker_id << "\"} "
           << w->websocket_active_connections.load(std::memory_order_relaxed) << "\n";
    }

    return ss.str();
}

} // namespace helios
