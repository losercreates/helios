#include <gtest/gtest.h>
#include "lb/weighted_round_robin.hpp"

using namespace helios;

TEST(WeightedRoundRobinTest, EqualWeights) {
    WeightedRoundRobinLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001, 1));
    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002, 1));
    lb.AddBackend(std::make_shared<Backend>(3, "127.0.0.1", 9003, 1));

    EXPECT_EQ(lb.GetBackendCount(), 3u);

    // Should behave like standard round-robin
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9003);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
}

TEST(WeightedRoundRobinTest, Weight1to2) {
    WeightedRoundRobinLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001, 1));
    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002, 2));

    // Expected sequence over 3 selections: 2, 1, 2
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
    
    // Pattern repeats
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
}

TEST(WeightedRoundRobinTest, Weight1to3) {
    WeightedRoundRobinLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001, 1));
    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002, 3));

    // Expected distribution over 4 selections: 3 for 9002, 1 for 9001.
    // Sequence: 9002, 9001, 9002, 9002
    int count1 = 0, count2 = 0;
    for (int i = 0; i < 4; ++i) {
        if (lb.SelectBackend()->GetPort() == 9001) count1++;
        else count2++;
    }
    EXPECT_EQ(count1, 1);
    EXPECT_EQ(count2, 3);
}

TEST(WeightedRoundRobinTest, Weight5to1) {
    WeightedRoundRobinLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001, 5));
    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002, 1));

    // Smooth WRR sequence over 6 selections should be: 1, 1, 1, 2, 1, 1
    int count1 = 0, count2 = 0;
    for (int i = 0; i < 6; ++i) {
        if (lb.SelectBackend()->GetPort() == 9001) count1++;
        else count2++;
    }
    EXPECT_EQ(count1, 5);
    EXPECT_EQ(count2, 1);
}

TEST(WeightedRoundRobinTest, ZeroWeight) {
    WeightedRoundRobinLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001, 0));
    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002, 1));

    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
}

TEST(WeightedRoundRobinTest, AllZeroWeights) {
    WeightedRoundRobinLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001, 0));
    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002, 0));

    EXPECT_EQ(lb.SelectBackend(), nullptr);
}

TEST(WeightedRoundRobinTest, OneBackend) {
    WeightedRoundRobinLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001, 5));

    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
}

TEST(WeightedRoundRobinTest, BackendRemoval) {
    WeightedRoundRobinLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001, 2));
    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002, 1));

    EXPECT_TRUE(lb.RemoveBackend(1));
    
    EXPECT_EQ(lb.GetBackendCount(), 1u);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
}

TEST(WeightedRoundRobinTest, BackendAddition) {
    WeightedRoundRobinLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001, 1));
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);

    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002, 2));
    
    // Now WRR includes backend 2
    int count2 = 0;
    for (int i = 0; i < 3; ++i) {
        if (lb.SelectBackend()->GetPort() == 9002) count2++;
    }
    EXPECT_EQ(count2, 2);
}

TEST(WeightedRoundRobinTest, DrainingAndIneligible) {
    WeightedRoundRobinLoadBalancer lb;
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 9001, 2);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 9002, 2);
    auto b3 = std::make_shared<Backend>(3, "127.0.0.1", 9003, 2);
    
    lb.AddBackend(b1);
    lb.AddBackend(b2);
    lb.AddBackend(b3);

    b1->SetEligible(false);
    b3->SetDraining(true);

    // Should only route to b2
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
}
