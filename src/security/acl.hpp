#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdint>
#include <shared_mutex>
#include <mutex>
#include <netinet/in.h>

namespace helios {

enum class AclAction : uint8_t {
    Allow,
    Deny
};

struct AclRule {
    uint32_t network_base{0}; // Host byte order
    uint32_t netmask{0};      // Host byte order
    uint8_t prefix_len{32};
    AclAction action{AclAction::Allow};
    std::string cidr;

    // Parses a CIDR string (e.g. "192.168.1.0/24" or "10.0.0.1") into an AclRule.
    // Returns std::nullopt if the format or prefix length is invalid.
    static std::optional<AclRule> Parse(std::string_view cidr_str, AclAction action);

    // Checks if the given IPv4 host-byte-order address matches this rule.
    [[nodiscard]] bool Matches(uint32_t host_ip) const noexcept {
        return (host_ip & netmask) == network_base;
    }
};

class IpAcl {
public:
    IpAcl() = default;
    explicit IpAcl(AclAction default_policy, bool enabled = true)
        : default_policy_(default_policy), enabled_(enabled) {}
    ~IpAcl() = default;

    // Movable with mutex synchronization
    IpAcl(IpAcl&& other) noexcept {
        std::unique_lock lock(other.mutex_);
        rules_ = std::move(other.rules_);
        default_policy_ = other.default_policy_;
        enabled_ = other.enabled_;
    }

    IpAcl& operator=(IpAcl&& other) noexcept {
        if (this != &other) {
            std::scoped_lock lock(mutex_, other.mutex_);
            rules_ = std::move(other.rules_);
            default_policy_ = other.default_policy_;
            enabled_ = other.enabled_;
        }
        return *this;
    }

    // Non-copyable
    IpAcl(const IpAcl&) = delete;
    IpAcl& operator=(const IpAcl&) = delete;

    void SetEnabled(bool enabled) noexcept {
        std::unique_lock lock(mutex_);
        enabled_ = enabled;
    }

    [[nodiscard]] bool IsEnabled() const noexcept {
        std::shared_lock lock(mutex_);
        return enabled_;
    }

    void SetDefaultPolicy(AclAction policy) noexcept {
        std::unique_lock lock(mutex_);
        default_policy_ = policy;
    }

    [[nodiscard]] AclAction GetDefaultPolicy() const noexcept {
        std::shared_lock lock(mutex_);
        return default_policy_;
    }

    void AddRule(AclRule rule) {
        std::unique_lock lock(mutex_);
        rules_.push_back(std::move(rule));
    }

    void ClearRules() noexcept {
        std::unique_lock lock(mutex_);
        rules_.clear();
    }

    [[nodiscard]] std::vector<AclRule> GetRules() const {
        std::shared_lock lock(mutex_);
        return rules_;
    }

    // Checks if client IP is permitted according to first matching rule or default policy.
    [[nodiscard]] bool IsAllowed(uint32_t host_ip) const;
    [[nodiscard]] bool IsAllowed(const sockaddr_in& addr) const;
    [[nodiscard]] bool IsAllowed(std::string_view ip_str) const;

private:
    mutable std::shared_mutex mutex_;
    std::vector<AclRule> rules_;
    AclAction default_policy_{AclAction::Allow};
    bool enabled_{true};
};

} // namespace helios
