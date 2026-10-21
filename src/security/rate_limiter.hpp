#pragma once

#include "security/security_config.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <netinet/in.h>

namespace helios {

class RateLimiter {
public:
    static constexpr size_t kDefaultNumShards = 64;

    RateLimiter();
    explicit RateLimiter(const RateLimitConfig& config, size_t num_shards = kDefaultNumShards);
    ~RateLimiter() = default;

    // Non-copyable, non-movable
    RateLimiter(const RateLimiter&) = delete;
    RateLimiter& operator=(const RateLimiter&) = delete;
    RateLimiter(RateLimiter&&) = delete;
    RateLimiter& operator=(RateLimiter&&) = delete;

    // Check if client IP has available token capacity. Consumes 1.0 token by default.
    [[nodiscard]] bool Allow(uint32_t host_ip, double cost = 1.0);
    [[nodiscard]] bool Allow(const sockaddr_in& addr, double cost = 1.0);
    [[nodiscard]] bool Allow(std::string_view ip_str, double cost = 1.0);

    // Reconfigure parameters dynamically
    void Configure(double rate_per_sec, uint32_t burst_size, uint8_t cidr_mask = 32);
    void SetEnabled(bool enabled) noexcept;
    [[nodiscard]] bool IsEnabled() const noexcept;

    // Reclaim memory for buckets that have been idle longer than max_idle_age
    size_t CleanupIdle(std::chrono::milliseconds max_idle_age = std::chrono::milliseconds(60000));

    // Clear all tracked buckets
    void Reset();

    // Query active tracked bucket count across all shards
    [[nodiscard]] size_t GetTrackedBucketCount() const;

private:
    [[nodiscard]] uint32_t MaskIp(uint32_t host_ip, uint8_t mask_bits) const noexcept;

    struct Bucket {
        double tokens{0.0};
        std::chrono::steady_clock::time_point last_refill;
    };

    struct Shard {
        mutable std::mutex mtx;
        std::unordered_map<uint32_t, Bucket> buckets;
    };

    std::atomic<bool> enabled_{false};
    std::atomic<double> rate_per_sec_{100.0};
    std::atomic<uint32_t> burst_size_{20};
    std::atomic<uint8_t> cidr_mask_{32};

    size_t num_shards_{kDefaultNumShards};
    std::vector<Shard> shards_;
};

} // namespace helios
