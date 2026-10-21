#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>

namespace helios {

struct AclRuleConfig {
    std::string cidr;
    std::string action{"allow"}; // "allow" or "deny"
};

struct AclConfig {
    bool enabled{false};
    std::string default_policy{"allow"}; // "allow" or "deny"
    std::vector<AclRuleConfig> rules;
};

struct RateLimitConfig {
    bool enabled{false};
    double rate_per_sec{100.0};
    uint32_t burst_size{20};
    uint8_t cidr_mask{32}; // 32 = per-IP, 24 = per /24 subnet, etc.
};

struct SlowlorisConfig {
    bool enabled{true};
    std::chrono::milliseconds header_read_timeout{5000};
};

struct SecurityConfig {
    AclConfig acl;
    RateLimitConfig rate_limit;
    SlowlorisConfig slowloris;
};

} // namespace helios
