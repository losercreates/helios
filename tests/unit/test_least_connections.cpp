#include <gtest/gtest.h>
#include "lb/least_connections.hpp"

using namespace helios;

TEST(LeastConnectionsTest, ZeroBackends) {
    LeastConnectionsLoadBalancer lb;
    EXPECT_EQ(lb.SelectBackend(), nullptr);
}

TEST(LeastConnectionsTest, OneBackend) {
    LeastConnectionsLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001));

    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
}

TEST(LeastConnectionsTest, EqualConnectionCounts) {
    LeastConnectionsLoadBalancer lb;
    lb.AddBackend(std::make_shared<Backend>(1, "127.0.0.1", 9001));
    lb.AddBackend(std::make_shared<Backend>(2, "127.0.0.1", 9002));
    
    // Deterministic tie-breaking (prefer lowest ID, which is 1)
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
}

TEST(LeastConnectionsTest, UnequalConnectionCounts) {
    LeastConnectionsLoadBalancer lb;
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 9001);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 9002);
    
    b1->IncrementActiveConnections(); // b1: 1, b2: 0
    
    lb.AddBackend(b1);
    lb.AddBackend(b2);

    // Should prefer b2 since it has fewer connections
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
}

TEST(LeastConnectionsTest, ConnectionCountIncrement) {
    LeastConnectionsLoadBalancer lb;
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 9001);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 9002);
    
    lb.AddBackend(b1);
    lb.AddBackend(b2);

    // Initial state: b1=0, b2=0 -> selects b1
    auto selected = lb.SelectBackend();
    EXPECT_EQ(selected->GetPort(), 9001);
    
    // Increment b1 count
    selected->IncrementActiveConnections(); // b1: 1, b2: 0
    
    // Next selection should be b2
    selected = lb.SelectBackend();
    EXPECT_EQ(selected->GetPort(), 9002);
    
    // Increment b2 count
    selected->IncrementActiveConnections(); // b1: 1, b2: 1
    
    // Next selection should be b1 again (tie-breaking)
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
}

TEST(LeastConnectionsTest, ConnectionCountDecrement) {
    LeastConnectionsLoadBalancer lb;
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 9001);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 9002);
    
    b1->IncrementActiveConnections();
    b1->IncrementActiveConnections(); // b1: 2, b2: 0
    
    b2->IncrementActiveConnections(); // b1: 2, b2: 1
    
    lb.AddBackend(b1);
    lb.AddBackend(b2);

    // b2 is chosen
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
    
    b1->DecrementActiveConnections(); 
    b1->DecrementActiveConnections(); // b1: 0, b2: 1
    
    // Now b1 is chosen
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9001);
}

TEST(LeastConnectionsTest, ProtectionAgainstNegativeCounts) {
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 9001);
    
    EXPECT_EQ(b1->GetActiveConnections(), 0u);
    b1->DecrementActiveConnections();
    EXPECT_EQ(b1->GetActiveConnections(), 0u); // Should not wrap around to UINT32_MAX
}

TEST(LeastConnectionsTest, DrainingBackend) {
    LeastConnectionsLoadBalancer lb;
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 9001);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 9002);
    
    b2->IncrementActiveConnections(); // b1: 0, b2: 1
    b1->SetDraining(true);
    
    lb.AddBackend(b1);
    lb.AddBackend(b2);

    // b1 has fewer connections, but is draining, so b2 must be chosen
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
}

TEST(LeastConnectionsTest, IneligibleBackend) {
    LeastConnectionsLoadBalancer lb;
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 9001);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 9002);
    
    b2->IncrementActiveConnections(); // b1: 0, b2: 1
    b1->SetEligible(false);
    
    lb.AddBackend(b1);
    lb.AddBackend(b2);

    // b1 has fewer connections, but is ineligible, so b2 must be chosen
    EXPECT_EQ(lb.SelectBackend()->GetPort(), 9002);
}

TEST(LeastConnectionsTest, ZeroEligibleBackends) {
    LeastConnectionsLoadBalancer lb;
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 9001);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 9002);
    
    b1->SetEligible(false);
    b2->SetDraining(true);
    
    lb.AddBackend(b1);
    lb.AddBackend(b2);

    EXPECT_EQ(lb.SelectBackend(), nullptr);
}
