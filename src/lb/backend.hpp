#pragma once

#include <string>
#include <netinet/in.h>
#include <cstdint>

namespace helios {

// Represents the state of a single backend endpoint.
// Identity, endpoint/address, configured weight, active connection count,
// eligibility state, draining state.
class Backend {
public:
    Backend(uint32_t id, std::string host, uint16_t port, uint32_t weight = 1);
    ~Backend() = default;

    // Backend Identity
    [[nodiscard]] uint32_t GetId() const noexcept { return id_; }
    [[nodiscard]] const std::string& GetHost() const noexcept { return host_; }
    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }
    [[nodiscard]] uint32_t GetWeight() const noexcept { return weight_; }

    // Pre-parsed sockaddr_in for fast hot-path usage without network/string operations
    [[nodiscard]] const sockaddr_in& GetSockAddr() const noexcept { return addr_; }

    // Eligibility Management
    [[nodiscard]] bool IsEligible() const noexcept { return eligible_; }
    void SetEligible(bool eligible) noexcept { eligible_ = eligible; }

    [[nodiscard]] bool IsDraining() const noexcept { return draining_; }
    void SetDraining(bool draining) noexcept { draining_ = draining; }

    // Active Connection Tracking
    void IncrementActiveConnections() noexcept { ++active_connections_; }
    void DecrementActiveConnections() noexcept { if (active_connections_ > 0) --active_connections_; }
    [[nodiscard]] uint32_t GetActiveConnections() const noexcept { return active_connections_; }

private:
    uint32_t id_;
    std::string host_;
    uint16_t port_;
    sockaddr_in addr_{};
    
    uint32_t weight_;
    
    bool eligible_{true};
    bool draining_{false};
    
    uint32_t active_connections_{0};
};

} // namespace helios
