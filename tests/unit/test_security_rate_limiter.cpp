#include <gtest/gtest.h>
#include "security/rate_limiter.hpp"
#include <thread>
#include <vector>
#include <chrono>

using namespace helios;

class SecurityRateLimiterTest : public ::testing::Test {};

TEST_F(SecurityRateLimiterTest, BurstExhaustion) {
    RateLimitConfig cfg;
    cfg.enabled = true;
    cfg.rate_per_sec = 10.0;
    cfg.burst_size = 5;
    cfg.cidr_mask = 32;

    RateLimiter limiter(cfg);

    // Initial burst of 5 should all pass
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(limiter.Allow("192.168.1.50"));
    }

    // 6th immediate request should be rejected
    EXPECT_FALSE(limiter.Allow("192.168.1.50"));
}

TEST_F(SecurityRateLimiterTest, TokenReplenishmentOverTime) {
    RateLimitConfig cfg;
    cfg.enabled = true;
    cfg.rate_per_sec = 20.0; // 20 tokens per second = 1 token every 50ms
    cfg.burst_size = 2;
    cfg.cidr_mask = 32;

    RateLimiter limiter(cfg);

    // Exhaust initial burst
    EXPECT_TRUE(limiter.Allow("10.0.0.1"));
    EXPECT_TRUE(limiter.Allow("10.0.0.1"));
    EXPECT_FALSE(limiter.Allow("10.0.0.1"));

    // Wait ~120ms to replenish ~2.4 tokens (capped at burst=2)
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    EXPECT_TRUE(limiter.Allow("10.0.0.1"));
    EXPECT_TRUE(limiter.Allow("10.0.0.1"));
    EXPECT_FALSE(limiter.Allow("10.0.0.1"));
}

TEST_F(SecurityRateLimiterTest, IndependentClientBuckets) {
    RateLimitConfig cfg;
    cfg.enabled = true;
    cfg.rate_per_sec = 5.0;
    cfg.burst_size = 2;
    cfg.cidr_mask = 32;

    RateLimiter limiter(cfg);

    // Exhaust client A
    EXPECT_TRUE(limiter.Allow("1.1.1.1"));
    EXPECT_TRUE(limiter.Allow("1.1.1.1"));
    EXPECT_FALSE(limiter.Allow("1.1.1.1"));

    // Client B should still have full burst capacity
    EXPECT_TRUE(limiter.Allow("2.2.2.2"));
    EXPECT_TRUE(limiter.Allow("2.2.2.2"));
    EXPECT_FALSE(limiter.Allow("2.2.2.2"));
}

TEST_F(SecurityRateLimiterTest, CidrSubnetGrouping) {
    RateLimitConfig cfg;
    cfg.enabled = true;
    cfg.rate_per_sec = 10.0;
    cfg.burst_size = 3;
    cfg.cidr_mask = 24; // All 192.168.1.x share the same bucket

    RateLimiter limiter(cfg);

    // 3 requests from different IPs in the same /24 subnet
    EXPECT_TRUE(limiter.Allow("192.168.1.10"));
    EXPECT_TRUE(limiter.Allow("192.168.1.20"));
    EXPECT_TRUE(limiter.Allow("192.168.1.30"));

    // 4th request from that subnet is rejected
    EXPECT_FALSE(limiter.Allow("192.168.1.40"));

    // Different subnet (192.168.2.x) has independent capacity
    EXPECT_TRUE(limiter.Allow("192.168.2.10"));
}

TEST_F(SecurityRateLimiterTest, DisabledLimiterAllowsAll) {
    RateLimitConfig cfg;
    cfg.enabled = false;
    cfg.rate_per_sec = 1.0;
    cfg.burst_size = 1;

    RateLimiter limiter(cfg);

    for (int i = 0; i < 20; ++i) {
        EXPECT_TRUE(limiter.Allow("127.0.0.1"));
    }
}

TEST_F(SecurityRateLimiterTest, IdleBucketReclamation) {
    RateLimitConfig cfg;
    cfg.enabled = true;
    cfg.rate_per_sec = 100.0;
    cfg.burst_size = 2;
    cfg.cidr_mask = 32;

    RateLimiter limiter(cfg);

    EXPECT_TRUE(limiter.Allow("10.0.0.1"));
    EXPECT_TRUE(limiter.Allow("10.0.0.2"));
    EXPECT_EQ(limiter.GetTrackedBucketCount(), 2u);

    // Immediate cleanup with 10s idle requirement cleans nothing
    size_t cleaned = limiter.CleanupIdle(std::chrono::seconds(10));
    EXPECT_EQ(cleaned, 0u);
    EXPECT_EQ(limiter.GetTrackedBucketCount(), 2u);

    // Wait 30ms for buckets to replenish back to burst_size=2
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Cleanup with 1ms idle requirement reclaims fully refilled buckets
    cleaned = limiter.CleanupIdle(std::chrono::milliseconds(1));
    EXPECT_EQ(cleaned, 2u);
    EXPECT_EQ(limiter.GetTrackedBucketCount(), 0u);
}

TEST_F(SecurityRateLimiterTest, MultiThreadedConcurrency) {
    RateLimitConfig cfg;
    cfg.enabled = true;
    cfg.rate_per_sec = 1000.0;
    cfg.burst_size = 500;
    cfg.cidr_mask = 32;

    RateLimiter limiter(cfg);

    constexpr int kNumThreads = 8;
    constexpr int kRequestsPerThread = 50;
    std::vector<std::thread> threads;
    std::atomic<bool> start_flag{false};
    std::atomic<size_t> allowed_count{0};

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&limiter, &start_flag, &allowed_count, t]() {
            while (!start_flag.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kRequestsPerThread; ++i) {
                // Each thread requests from its own IP
                std::string ip = "10.0.0." + std::to_string(t + 1);
                if (limiter.Allow(ip)) {
                    allowed_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    start_flag.store(true, std::memory_order_release);
    for (auto& th : threads) {
        th.join();
    }

    // Since burst_size is 500 per IP, all 50 requests per thread should be allowed
    EXPECT_EQ(allowed_count.load(), static_cast<size_t>(kNumThreads * kRequestsPerThread));
}
