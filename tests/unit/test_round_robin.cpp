#include <gtest/gtest.h>
#include "lb/round_robin.hpp"

using namespace helios;

TEST(RoundRobinTest, EmptyLoadBalancerReturnsNullopt) {
    RoundRobinLoadBalancer lb;
    EXPECT_TRUE(lb.IsEmpty());
    EXPECT_EQ(lb.GetBackendCount(), 0u);
    EXPECT_EQ(lb.SelectBackend(), nullptr);
}

TEST(RoundRobinTest, SingleBackendReturnsRepeatedly) {
    RoundRobinLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001));
    EXPECT_FALSE(lb.IsEmpty());
    EXPECT_EQ(lb.GetBackendCount(), 1u);

    auto ep1 = lb.SelectBackend();
    ASSERT_NE(ep1, nullptr);
    EXPECT_EQ(ep1->GetHost(), "127.0.0.1");
    EXPECT_EQ(ep1->GetPort(), 9001);

    auto ep2 = lb.SelectBackend();
    ASSERT_NE(ep2, nullptr);
    EXPECT_EQ(ep2->GetPort(), 9001);
}

TEST(RoundRobinTest, MultipleBackendsCycleInOrder) {
    RoundRobinLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001));
    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002));
    lb.AddBackend(std::make_shared<Backend>(3, "127.0.0.1", 9003));

    EXPECT_EQ(lb.GetBackendCount(), 3u);

    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9003);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001); // Wrap around
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
}

TEST(RoundRobinTest, BackendRemoval) {
    RoundRobinLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001));
    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002));
    lb.AddBackend(std::make_shared<Backend>(3, "127.0.0.1", 9003));

    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
    
    // Remove backend 2
    EXPECT_TRUE(lb.RemoveBackend(2));
    EXPECT_EQ(lb.GetBackendCount(), 2u);

    // Next should be 3, then 1, skipping 2
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9003);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
}

TEST(RoundRobinTest, IneligibleAndDrainingSkips) {
    RoundRobinLoadBalancer lb;
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 9001);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 9002);
    auto b3 = std::make_shared<Backend>(3, "127.0.0.1", 9003);
    
    lb.AddBackend(b1);
    lb.AddBackend(b2);
    lb.AddBackend(b3);

    b2->SetEligible(false);
    b3->SetDraining(true);

    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
    // Skips 2 (ineligible) and 3 (draining), wraps to 1
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);

    // Restore 2
    b2->SetEligible(true);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
}

TEST(RoundRobinTest, AllBackendsUnavailable) {
    RoundRobinLoadBalancer lb;
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 9001);
    b1->SetEligible(false);
    lb.AddBackend(b1);

    EXPECT_EQ(lb.SelectBackend(), nullptr);
}

TEST(RoundRobinTest, PreParsedSockAddr) {
    RoundRobinLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 8080));

    auto backend = lb.SelectBackend();
    ASSERT_NE(backend, nullptr);
    
    auto addr = backend->GetSockAddr();
    EXPECT_EQ(ntohs(addr.sin_port), 8080);
}
