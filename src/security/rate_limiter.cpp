#include "security/rate_limiter.hpp"

#include <arpa/inet.h>
#include <algorithm>

namespace helios {

RateLimiter::RateLimiter()
    : RateLimiter(RateLimitConfig{}, kDefaultNumShards) {}

RateLimiter::RateLimiter(const RateLimitConfig& config, size_t num_shards)
    : enabled_(config.enabled),
      rate_per_sec_(config.rate_per_sec),
      burst_size_(config.burst_size),
      cidr_mask_(config.cidr_mask),
      num_shards_(num_shards == 0 ? 1 : num_shards),
      shards_(num_shards_ == 0 ? 1 : num_shards_) {}

uint32_t RateLimiter::MaskIp(uint32_t host_ip, uint8_t mask_bits) const noexcept {
    if (mask_bits == 0) return 0;
    if (mask_bits >= 32) return host_ip;
    uint32_t mask = (~0U) << (32 - mask_bits);
    return host_ip & mask;
}

bool RateLimiter::Allow(uint32_t host_ip, double cost) {
    if (!enabled_.load(std::memory_order_relaxed)) {
        return true;
    }
    if (cost <= 0.0) {
        return true;
    }

    uint32_t masked = MaskIp(host_ip, cidr_mask_.load(std::memory_order_relaxed));
    size_t shard_idx = std::hash<uint32_t>{}(masked) % num_shards_;
    auto& shard = shards_[shard_idx];

    std::lock_guard<std::mutex> lock(shard.mtx);
    auto now = std::chrono::steady_clock::now();
    double max_tokens = static_cast<double>(burst_size_.load(std::memory_order_relaxed));
    double rate = rate_per_sec_.load(std::memory_order_relaxed);

    auto it = shard.buckets.find(masked);
    if (it == shard.buckets.end()) {
        if (max_tokens < cost) {
            return false;
        }
        Bucket b;
        b.tokens = max_tokens - cost;
        b.last_refill = now;
        shard.buckets[masked] = b;
        return true;
    }

    Bucket& b = it->second;
    double elapsed_sec = std::chrono::duration<double>(now - b.last_refill).count();
    b.tokens = std::min(max_tokens, b.tokens + elapsed_sec * rate);
    b.last_refill = now;

    if (b.tokens >= cost) {
        b.tokens -= cost;
        return true;
    }
    return false;
}

bool RateLimiter::Allow(const sockaddr_in& addr, double cost) {
    uint32_t host_ip = ntohl(addr.sin_addr.s_addr);
    return Allow(host_ip, cost);
}

bool RateLimiter::Allow(std::string_view ip_str, double cost) {
    std::string ip_copy(ip_str);
    in_addr in4{};
    if (::inet_pton(AF_INET, ip_copy.c_str(), &in4) != 1) {
        // If IP string cannot be parsed, allow if disabled, deny if enabled
        return !enabled_.load(std::memory_order_relaxed);
    }
    uint32_t host_ip = ntohl(in4.s_addr);
    return Allow(host_ip, cost);
}

void RateLimiter::Configure(double rate_per_sec, uint32_t burst_size, uint8_t cidr_mask) {
    rate_per_sec_.store(rate_per_sec, std::memory_order_relaxed);
    burst_size_.store(burst_size, std::memory_order_relaxed);
    cidr_mask_.store(cidr_mask > 32 ? 32 : cidr_mask, std::memory_order_relaxed);
}

void RateLimiter::SetEnabled(bool enabled) noexcept {
    enabled_.store(enabled, std::memory_order_release);
}

bool RateLimiter::IsEnabled() const noexcept {
    return enabled_.load(std::memory_order_acquire);
}

size_t RateLimiter::CleanupIdle(std::chrono::milliseconds max_idle_age) {
    auto now = std::chrono::steady_clock::now();
    double max_tokens = static_cast<double>(burst_size_.load(std::memory_order_relaxed));
    double rate = rate_per_sec_.load(std::memory_order_relaxed);
    size_t removed = 0;

    for (auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mtx);
        for (auto it = shard.buckets.begin(); it != shard.buckets.end(); ) {
            auto idle_dur = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.last_refill);
            double elapsed_sec = std::chrono::duration<double>(now - it->second.last_refill).count();
            double current_tokens = std::min(max_tokens, it->second.tokens + elapsed_sec * rate);
            if (idle_dur >= max_idle_age && current_tokens >= (max_tokens - 0.001)) {
                it = shard.buckets.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
    }
    return removed;
}

void RateLimiter::Reset() {
    for (auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mtx);
        shard.buckets.clear();
    }
}

size_t RateLimiter::GetTrackedBucketCount() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard.mtx);
        total += shard.buckets.size();
    }
    return total;
}

} // namespace helios
