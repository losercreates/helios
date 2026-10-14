#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>

namespace helios {

// Configuration for BackendConnectionPool
struct BackendPoolConfig {
    size_t max_idle_per_backend{32};
    size_t max_total_idle{1024};
    std::chrono::milliseconds idle_timeout{60000}; // 60 seconds
};

// Represents an idle keep-alive backend connection
struct PooledConnection {
    int fd{-1};
    uint32_t backend_id{0};
    std::chrono::steady_clock::time_point idle_since{};
};

// Thread-isolated HTTP keep-alive backend connection pool
// Enforces shared-nothing architecture per worker with zero cross-thread locking.
class BackendConnectionPool {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using SocketCloser = std::function<void(int)>;

    explicit BackendConnectionPool(BackendPoolConfig config = {},
                                   SocketCloser closer = nullptr) noexcept;
    ~BackendConnectionPool();

    // Prevent copies, allow moves
    BackendConnectionPool(const BackendConnectionPool&) = delete;
    BackendConnectionPool& operator=(const BackendConnectionPool&) = delete;
    BackendConnectionPool(BackendConnectionPool&& other) noexcept;
    BackendConnectionPool& operator=(BackendConnectionPool&& other) noexcept;

    // Acquire an idle connection for backend_id.
    // Returns the socket fd if available and unexpired, or std::nullopt.
    [[nodiscard]] std::optional<int> Acquire(uint32_t backend_id,
                                             TimePoint now = Clock::now());

    // Release an active socket back to the pool as idle.
    // Returns true if retained, false if rejected or closed due to invalidity/error.
    bool Release(uint32_t backend_id, int fd, TimePoint now = Clock::now());

    // Purge and close all pooled connections associated with backend_id (e.g. on draining/unhealthy)
    size_t Purge(uint32_t backend_id);

    // Reap and close all connections across all backends that have exceeded idle_timeout
    size_t ReapExpired(TimePoint now = Clock::now());

    // Close and remove all pooled connections across all backends
    void Clear();

    // Queries
    [[nodiscard]] size_t TotalIdleCount() const noexcept { return total_idle_count_; }
    [[nodiscard]] size_t IdleCountForBackend(uint32_t backend_id) const noexcept;
    [[nodiscard]] const BackendPoolConfig& GetConfig() const noexcept { return config_; }
    void SetConfig(BackendPoolConfig config) noexcept { config_ = config; }

private:
    void CloseFd(int fd) noexcept;
    void EvictOldestGlobally();

    BackendPoolConfig config_;
    SocketCloser closer_;
    size_t total_idle_count_{0};

    // Mapping: backend_id -> deque of pooled connections (oldest at front, newest at back)
    std::unordered_map<uint32_t, std::deque<PooledConnection>> pools_;
};

} // namespace helios
