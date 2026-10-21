#include "security/acl.hpp"

#include <arpa/inet.h>
#include <charconv>
#include <cstring>

namespace helios {

std::optional<AclRule> AclRule::Parse(std::string_view cidr_str, AclAction action) {
    if (cidr_str.empty()) {
        return std::nullopt;
    }

    std::string_view ip_part = cidr_str;
    uint8_t prefix = 32;

    auto slash_pos = cidr_str.find('/');
    if (slash_pos != std::string_view::npos) {
        ip_part = cidr_str.substr(0, slash_pos);
        std::string_view prefix_part = cidr_str.substr(slash_pos + 1);
        if (prefix_part.empty()) {
            return std::nullopt;
        }

        unsigned int p_val = 0;
        auto [ptr, ec] = std::from_chars(prefix_part.data(), prefix_part.data() + prefix_part.size(), p_val);
        if (ec != std::errc{} || ptr != prefix_part.data() + prefix_part.size() || p_val > 32) {
            return std::nullopt;
        }
        prefix = static_cast<uint8_t>(p_val);
    }

    std::string ip_copy(ip_part);
    in_addr in4{};
    if (::inet_pton(AF_INET, ip_copy.c_str(), &in4) != 1) {
        return std::nullopt;
    }

    uint32_t host_ip = ntohl(in4.s_addr);
    uint32_t mask = 0;
    if (prefix == 32) {
        mask = 0xFFFFFFFFU;
    } else if (prefix > 0) {
        mask = (~0U) << (32 - prefix);
    }

    AclRule rule;
    rule.netmask = mask;
    rule.network_base = host_ip & mask;
    rule.prefix_len = prefix;
    rule.action = action;
    rule.cidr = std::string(cidr_str);
    return rule;
}

bool IpAcl::IsAllowed(uint32_t host_ip) const {
    std::shared_lock lock(mutex_);
    if (!enabled_) {
        return true;
    }

    for (const auto& rule : rules_) {
        if (rule.Matches(host_ip)) {
            return rule.action == AclAction::Allow;
        }
    }

    return default_policy_ == AclAction::Allow;
}

bool IpAcl::IsAllowed(const sockaddr_in& addr) const {
    uint32_t host_ip = ntohl(addr.sin_addr.s_addr);
    return IsAllowed(host_ip);
}

bool IpAcl::IsAllowed(std::string_view ip_str) const {
    std::string ip_copy(ip_str);
    in_addr in4{};
    if (::inet_pton(AF_INET, ip_copy.c_str(), &in4) != 1) {
        // Fail-safe: malformed or unresolvable IP is denied when ACL is enabled
        std::shared_lock lock(mutex_);
        if (!enabled_) return true;
        return false;
    }

    uint32_t host_ip = ntohl(in4.s_addr);
    return IsAllowed(host_ip);
}

} // namespace helios
