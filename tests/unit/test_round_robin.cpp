#include <gtest/gtest.h>
#include "lb/round_robin.hpp"

using namespace helios;

TEST(RoundRobinTest, EmptyLoadBalancerReturnsNullopt) {
    RoundRobinLoadBalancer lb;
    EXPECT_TRUE(lb.IsEmpty());
    EXPECT_EQ(lb.GetBackendCount(), 0u);
    EXPECT_EQ(lb.SelectBackend(), std::nullopt);
}

TEST(RoundRobinTest, SingleBackendReturnsRepeatedly) {
    RoundRobinLoadBalancer lb;
    lb.AddBackend("127.0.0.1", 9001);
    EXPECT_FALSE(lb.IsEmpty());
    EXPECT_EQ(lb.GetBackendCount(), 1u);

    auto ep1 = lb.SelectBackend();
    ASSERT_TRUE(ep1.has_value());
    EXPECT_EQ(ep1->host, "127.0.0.1");
    EXPECT_EQ(ep1->port, 9001);

    auto ep2 = lb.SelectBackend();
    ASSERT_TRUE(ep2.has_value());
    EXPECT_EQ(ep2->port, 9001);
}

TEST(RoundRobinTest, MultipleBackendsCycleInOrder) {
    RoundRobinLoadBalancer lb;
    lb.AddBackend("127.0.0.1", 9001);
    lb.AddBackend("127.0.0.1", 9002);
    lb.AddBackend("127.0.0.1", 9003);

    EXPECT_EQ(lb.GetBackendCount(), 3u);

    EXPECT_EQ(lb.SelectBackend()->port, 9001);
    EXPECT_EQ(lb.SelectBackend()->port, 9002);
    EXPECT_EQ(lb.SelectBackend()->port, 9003);
    EXPECT_EQ(lb.SelectBackend()->port, 9001); // Wrap around
    EXPECT_EQ(lb.SelectBackend()->port, 9002);
}

TEST(RoundRobinTest, SelectBackendAddrParsesSockAddr) {
    RoundRobinLoadBalancer lb;
    lb.AddBackend("127.0.0.1", 8080);

    sockaddr_in addr{};
    BackendEndpoint ep{};
    bool success = lb.SelectBackendAddr(&addr, &ep);
    EXPECT_TRUE(success);
    EXPECT_EQ(ep.port, 8080);
    EXPECT_EQ(ntohs(addr.sin_port), 8080);
}
