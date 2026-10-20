#include "backend_pool.hpp"
#include <unistd.h>
#include <algorithm>
#include <sys/socket.h>
#include <cerrno>

namespace helios {

BackendConnectionPool::BackendConnectionPool(BackendConnectionPoolConfig config, SocketCloser closer) noexcept
    : config_(config), closer_(std::move(closer)) {
}

BackendConnectionPool::~BackendConnectionPool() {
    Clear();
}

BackendConnectionPool::BackendConnectionPool(BackendConnectionPool&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.mutex_);
    config_ = other.config_;
    closer_ = std::move(other.closer_);
    total_idle_count_ = other.total_idle_count_;
    pools_ = std::move(other.pools_);
    other.total_idle_count_ = 0;
}

BackendConnectionPool& BackendConnectionPool::operator=(BackendConnectionPool&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(mutex_, other.mutex_);
        ClearLocked();
        config_ = other.config_;
        closer_ = std::move(other.closer_);
        total_idle_count_ = other.total_idle_count_;
        pools_ = std::move(other.pools_);
        other.total_idle_count_ = 0;
    }
    return *this;
}

void BackendConnectionPool::CloseFd(int fd) noexcept {
    if (fd < 0) return;
    if (closer_) {
        closer_(fd);
    } else {
        ::close(fd);
    }
}

size_t BackendConnectionPool::TotalIdleCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_idle_count_;
}

size_t BackendConnectionPool::IdleCountForBackend(uint32_t backend_id) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pools_.find(backend_id);
    if (it == pools_.end()) {
        return 0;
    }
    return it->second.size();
}

BackendConnectionPoolConfig BackendConnectionPool::GetConfig() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void BackendConnectionPool::SetConfig(BackendConnectionPoolConfig config) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

void BackendConnectionPool::EvictOldestGlobally() {
    if (pools_.empty() || total_idle_count_ == 0) {
        return;
    }

    uint32_t oldest_backend = 0;
    bool found = false;
    TimePoint earliest_time = TimePoint::max();

    for (const auto& [b_id, pool] : pools_) {
        if (!pool.empty()) {
            if (!found || pool.front().idle_since < earliest_time) {
                found = true;
                earliest_time = pool.front().idle_since;
                oldest_backend = b_id;
            }
        }
    }

    if (found) {
        auto& pool = pools_[oldest_backend];
        int fd = pool.front().fd;
        pool.pop_front();
        if (total_idle_count_ > 0) {
            --total_idle_count_;
        }
        CloseFd(fd);
        if (pool.empty()) {
            pools_.erase(oldest_backend);
        }
    }
}

std::optional<int> BackendConnectionPool::Acquire(uint32_t backend_id, TimePoint now) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pools_.find(backend_id);
    if (it == pools_.end()) {
        return std::nullopt;
    }

    auto& pool = it->second;
    while (!pool.empty()) {
        // LIFO: reuse most recently active connection from back of deque
        PooledConnection conn = pool.back();
        pool.pop_back();
        if (total_idle_count_ > 0) {
            --total_idle_count_;
        }

        // Check if connection has expired while idle
        if (now - conn.idle_since >= config_.idle_timeout) {
            CloseFd(conn.fd);
            continue;
        }

        // Liveness & Cleanliness Check: verify socket was not closed, reset, or dirty while idle
        char peek_buf;
        ssize_t peek_ret = ::recv(conn.fd, &peek_buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (peek_ret == 0 || (peek_ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EBADF && errno != ENOTSOCK)) {
            // Peer closed (EOF) or connection reset/errored while idle
            CloseFd(conn.fd);
            continue;
        }
        if (peek_ret > 0) {
            // Unexpected unread data pending while supposed to be idle
            CloseFd(conn.fd);
            continue;
        }

        if (pool.empty()) {
            pools_.erase(it);
        }
        return conn.fd;
    }

    pools_.erase(it);
    return std::nullopt;
}

bool BackendConnectionPool::Release(uint32_t backend_id, int fd, TimePoint now) {
    if (fd < 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (config_.max_idle_per_backend == 0 || config_.max_total_idle == 0) {
        CloseFd(fd);
        return false;
    }

    // 1. Enforce global capacity limit
    while (total_idle_count_ >= config_.max_total_idle) {
        EvictOldestGlobally();
    }

    // 2. Enforce per-backend capacity limit
    auto& pool = pools_[backend_id];
    while (pool.size() >= config_.max_idle_per_backend) {
        int oldest_fd = pool.front().fd;
        pool.pop_front();
        if (total_idle_count_ > 0) {
            --total_idle_count_;
        }
        CloseFd(oldest_fd);
    }

    // 3. Add to pool (newest at back)
    pool.push_back(PooledConnection{
        .fd = fd,
        .backend_id = backend_id,
        .idle_since = now
    });
    ++total_idle_count_;

    return true;
}

size_t BackendConnectionPool::Purge(uint32_t backend_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pools_.find(backend_id);
    if (it == pools_.end()) {
        return 0;
    }

    auto& pool = it->second;
    size_t count = pool.size();
    for (const auto& conn : pool) {
        CloseFd(conn.fd);
    }

    if (total_idle_count_ >= count) {
        total_idle_count_ -= count;
    } else {
        total_idle_count_ = 0;
    }

    pools_.erase(it);
    return count;
}

size_t BackendConnectionPool::ReapExpired(TimePoint now) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t reaped_count = 0;

    for (auto it = pools_.begin(); it != pools_.end();) {
        auto& pool = it->second;
        // Connections are ordered chronologically (oldest at front)
        while (!pool.empty()) {
            if (now - pool.front().idle_since >= config_.idle_timeout) {
                int fd = pool.front().fd;
                pool.pop_front();
                if (total_idle_count_ > 0) {
                    --total_idle_count_;
                }
                ++reaped_count;
                CloseFd(fd);
            } else {
                // Since connections are ordered, remaining are newer
                break;
            }
        }

        if (pool.empty()) {
            it = pools_.erase(it);
        } else {
            ++it;
        }
    }

    return reaped_count;
}

void BackendConnectionPool::ClearLocked() noexcept {
    for (auto& [b_id, pool] : pools_) {
        (void)b_id;
        for (const auto& conn : pool) {
            CloseFd(conn.fd);
        }
    }
    pools_.clear();
    total_idle_count_ = 0;
}

void BackendConnectionPool::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    ClearLocked();
}

} // namespace helios
