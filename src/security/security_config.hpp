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

struct ResponseHeadersConfig {
    bool enabled{false};
    bool set_server_header{true};
    std::string server_value{"helios"};
    bool hsts_enabled{false};
    std::string hsts_value{"max-age=31536000; includeSubDomains"};
    bool x_content_type_options{true}; // "nosniff"
    std::string x_frame_options{"SAMEORIGIN"}; // "DENY", "SAMEORIGIN", etc.
    std::vector<std::pair<std::string, std::string>> custom_headers;
};

struct SecurityConfig {
    AclConfig acl;
    RateLimitConfig rate_limit;
    SlowlorisConfig slowloris;
    ResponseHeadersConfig headers;
};

} // namespace helios
