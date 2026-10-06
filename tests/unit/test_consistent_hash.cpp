#include <gtest/gtest.h>
#include "lb/consistent_hash.hpp"
#include <unordered_map>
#include <cmath>

using namespace helios;

TEST(ConsistentHashTest, ZeroEligibleBackends) {
    ConsistentHashLoadBalancer lb;
    EXPECT_EQ(lb.SelectBackendByKey(12345), nullptr);
    EXPECT_EQ(lb.SelectBackend(), nullptr);
}

TEST(ConsistentHashTest, OneBackend) {
    ConsistentHashLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001));

    EXPECT_EQ(lb.SelectBackendByKey(123)->GetPort(), 9001);
    EXPECT_EQ(lb.SelectBackendByKey(999)->GetPort(), 9001);
}

TEST(ConsistentHashTest, DeterministicKeyMapping) {
    ConsistentHashLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001));
    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002));
    lb.AddBackend(std::make_shared<Backend>(3, "127.0.0.1", 9003));

    uint64_t key1 = 10001;
    uint64_t key2 = 10002;

    auto b1 = lb.SelectBackendByKey(key1);
    auto b2 = lb.SelectBackendByKey(key2);

    // Repeated identical keys map to the same backend
    EXPECT_EQ(lb.SelectBackendByKey(key1)->GetPort(), b1->GetPort());
    EXPECT_EQ(lb.SelectBackendByKey(key1)->GetPort(), b1->GetPort());

    EXPECT_EQ(lb.SelectBackendByKey(key2)->GetPort(), b2->GetPort());
}

TEST(ConsistentHashTest, BackendAdditionRemapSubset) {
    ConsistentHashLoadBalancer lb(1000); // Higher virtual nodes for smooth distribution
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001));
    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002));
    lb.AddBackend(std::make_shared<Backend>(3, "127.0.0.1", 9003));

    std::unordered_map<uint64_t, uint16_t> initial_map;
    for (uint64_t k = 0; k < 1000; ++k) {
        uint64_t hashed_k = std::hash<std::string>{}(std::to_string(k));
        initial_map[k] = lb.SelectBackendByKey(hashed_k)->GetPort();
    }

    // Add a 4th backend
    lb.AddBackend(std::make_shared<Backend>(4, "127.0.0.1", 9004));

    int remapped = 0;
    for (uint64_t k = 0; k < 1000; ++k) {
        uint64_t hashed_k = std::hash<std::string>{}(std::to_string(k));
        if (lb.SelectBackendByKey(hashed_k)->GetPort() != initial_map[k]) {
            remapped++;
        }
    }

    // Roughly 1/4 of keys should be remapped to the new node.
    // Allow statistical tolerance [150, 350]
    EXPECT_GE(remapped, 150);
    EXPECT_LE(remapped, 350);
}

TEST(ConsistentHashTest, BackendRemovalRemapDependentKeys) {
    ConsistentHashLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001));
    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002));
    lb.AddBackend(std::make_shared<Backend>(3, "127.0.0.1", 9003));

    std::unordered_map<uint64_t, uint16_t> initial_map;
    for (uint64_t k = 0; k < 1000; ++k) {
        uint64_t hashed_k = std::hash<std::string>{}(std::to_string(k));
        initial_map[k] = lb.SelectBackendByKey(hashed_k)->GetPort();
    }

    // Remove backend 3
    lb.RemoveBackend(3);

    int remapped = 0;
    int expected_remapped = 0;
    for (uint64_t k = 0; k < 1000; ++k) {
        uint64_t hashed_k = std::hash<std::string>{}(std::to_string(k));
        if (initial_map[k] == 9003) {
            expected_remapped++;
        }
        if (lb.SelectBackendByKey(hashed_k)->GetPort() != initial_map[k]) {
            remapped++;
            // Only keys that mapped to 9003 should have changed
            EXPECT_EQ(initial_map[k], 9003);
        }
    }

    EXPECT_EQ(remapped, expected_remapped);
}

TEST(ConsistentHashTest, BackendBecomingIneligible) {
    ConsistentHashLoadBalancer lb;
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 9001);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 9002);
    lb.AddBackend(b1);
    lb.AddBackend(b2);

    uint64_t test_key = 555;
    auto initial_b = lb.SelectBackendByKey(test_key);
    
    // Make the mapped backend ineligible
    initial_b->SetEligible(false);

    // Should seamlessly map to the other backend without ring rebuild
    auto new_b = lb.SelectBackendByKey(test_key);
    EXPECT_NE(new_b->GetPort(), initial_b->GetPort());
    EXPECT_TRUE(new_b->IsEligible());
}

TEST(ConsistentHashTest, StatisticalDistribution) {
    ConsistentHashLoadBalancer lb(1000); // 1000 virtual nodes for good distribution
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001));
    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002));
    lb.AddBackend(std::make_shared<Backend>(3, "127.0.0.1", 9003));

    std::unordered_map<uint16_t, int> counts;
    int num_keys = 10000;
    for (uint64_t k = 0; k < static_cast<uint64_t>(num_keys); ++k) {
        uint64_t hashed_k = std::hash<std::string>{}(std::to_string(k));
        counts[lb.SelectBackendByKey(hashed_k)->GetPort()]++;
    }

    double expected = num_keys / 3.0;
    double tolerance = expected * 0.25; // allow 25% variance

    for (const auto& [port, count] : counts) {
        EXPECT_GE(count, expected - tolerance);
        EXPECT_LE(count, expected + tolerance);
    }
}
