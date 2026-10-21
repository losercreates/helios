#include <gtest/gtest.h>
#include "security/acl.hpp"
#include <thread>
#include <vector>

using namespace helios;

class SecurityAclTest : public ::testing::Test {};

TEST_F(SecurityAclTest, ParseValidCidrs) {
    auto r1 = AclRule::Parse("192.168.1.0/24", AclAction::Allow);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(r1->prefix_len, 24);
    EXPECT_EQ(r1->action, AclAction::Allow);
    EXPECT_EQ(r1->netmask, 0xFFFFFF00U);
    EXPECT_EQ(r1->network_base, 0xC0A80100U);

    auto r2 = AclRule::Parse("10.0.0.1", AclAction::Deny); // Implicit /32
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->prefix_len, 32);
    EXPECT_EQ(r2->action, AclAction::Deny);
    EXPECT_EQ(r2->netmask, 0xFFFFFFFFU);
    EXPECT_EQ(r2->network_base, 0x0A000001U);

    auto r3 = AclRule::Parse("0.0.0.0/0", AclAction::Allow); // Catch-all
    ASSERT_TRUE(r3.has_value());
    EXPECT_EQ(r3->prefix_len, 0);
    EXPECT_EQ(r3->netmask, 0U);
    EXPECT_EQ(r3->network_base, 0U);
}

TEST_F(SecurityAclTest, ParseInvalidCidrs) {
    EXPECT_FALSE(AclRule::Parse("", AclAction::Allow).has_value());
    EXPECT_FALSE(AclRule::Parse("not-an-ip", AclAction::Allow).has_value());
    EXPECT_FALSE(AclRule::Parse("192.168.1.1/33", AclAction::Allow).has_value());
    EXPECT_FALSE(AclRule::Parse("192.168.1.1/abc", AclAction::Allow).has_value());
    EXPECT_FALSE(AclRule::Parse("192.168.1.1/", AclAction::Allow).has_value());
    EXPECT_FALSE(AclRule::Parse("256.0.0.1/24", AclAction::Allow).has_value());
}

TEST_F(SecurityAclTest, DefaultAllowPolicy) {
    IpAcl acl(AclAction::Allow);
    // No rules: default allow allows all
    EXPECT_TRUE(acl.IsAllowed("127.0.0.1"));
    EXPECT_TRUE(acl.IsAllowed("10.1.2.3"));
    EXPECT_TRUE(acl.IsAllowed("192.168.1.100"));

    // Add deny rule for 10.0.0.0/8
    auto rule = AclRule::Parse("10.0.0.0/8", AclAction::Deny);
    ASSERT_TRUE(rule.has_value());
    acl.AddRule(std::move(*rule));

    EXPECT_FALSE(acl.IsAllowed("10.0.0.1"));
    EXPECT_FALSE(acl.IsAllowed("10.255.255.255"));
    EXPECT_TRUE(acl.IsAllowed("11.0.0.1"));
    EXPECT_TRUE(acl.IsAllowed("127.0.0.1"));
}

TEST_F(SecurityAclTest, DefaultDenyPolicy) {
    IpAcl acl(AclAction::Deny);
    // No rules: default deny blocks all
    EXPECT_FALSE(acl.IsAllowed("127.0.0.1"));
    EXPECT_FALSE(acl.IsAllowed("10.1.2.3"));

    // Add allow rule for 192.168.1.0/24
    auto rule = AclRule::Parse("192.168.1.0/24", AclAction::Allow);
    ASSERT_TRUE(rule.has_value());
    acl.AddRule(std::move(*rule));

    EXPECT_TRUE(acl.IsAllowed("192.168.1.1"));
    EXPECT_TRUE(acl.IsAllowed("192.168.1.254"));
    EXPECT_FALSE(acl.IsAllowed("192.168.2.1"));
    EXPECT_FALSE(acl.IsAllowed("127.0.0.1"));
}

TEST_F(SecurityAclTest, RuleEvaluationOrder) {
    IpAcl acl(AclAction::Deny);

    // Rule 1: Allow specific host 10.0.0.50
    acl.AddRule(*AclRule::Parse("10.0.0.50/32", AclAction::Allow));
    // Rule 2: Deny entire subnet 10.0.0.0/16
    acl.AddRule(*AclRule::Parse("10.0.0.0/16", AclAction::Deny));
    // Rule 3: Allow everything else in 10.0.0.0/8
    acl.AddRule(*AclRule::Parse("10.0.0.0/8", AclAction::Allow));

    // 10.0.0.50 hits Rule 1 -> Allow
    EXPECT_TRUE(acl.IsAllowed("10.0.0.50"));
    // 10.0.0.51 hits Rule 2 -> Deny
    EXPECT_FALSE(acl.IsAllowed("10.0.0.51"));
    // 10.1.0.1 misses Rule 1 & 2, hits Rule 3 -> Allow
    EXPECT_TRUE(acl.IsAllowed("10.1.0.1"));
    // 192.168.1.1 misses all rules -> Default Deny
    EXPECT_FALSE(acl.IsAllowed("192.168.1.1"));
}

TEST_F(SecurityAclTest, DisabledAclAllowsAll) {
    IpAcl acl(AclAction::Deny, /*enabled=*/false);
    acl.AddRule(*AclRule::Parse("10.0.0.0/8", AclAction::Deny));

    EXPECT_TRUE(acl.IsAllowed("10.0.0.1"));
    EXPECT_TRUE(acl.IsAllowed("192.168.1.1"));

    acl.SetEnabled(true);
    EXPECT_FALSE(acl.IsAllowed("10.0.0.1"));
}

TEST_F(SecurityAclTest, ThreadSafetyAndConcurrentAccess) {
    IpAcl acl(AclAction::Allow);
    acl.AddRule(*AclRule::Parse("10.0.0.1/32", AclAction::Allow));
    acl.AddRule(*AclRule::Parse("10.0.0.0/8", AclAction::Deny));

    constexpr int kNumThreads = 8;
    constexpr int kIterations = 1000;
    std::vector<std::thread> readers;
    std::atomic<bool> start_flag{false};

    for (int t = 0; t < kNumThreads; ++t) {
        readers.emplace_back([&acl, &start_flag]() {
            while (!start_flag.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kIterations; ++i) {
                EXPECT_TRUE(acl.IsAllowed("10.0.0.1"));
                EXPECT_FALSE(acl.IsAllowed("10.0.0.2"));
                EXPECT_TRUE(acl.IsAllowed("192.168.1.1"));
            }
        });
    }

    start_flag.store(true, std::memory_order_release);
    for (auto& th : readers) {
        th.join();
    }
}
