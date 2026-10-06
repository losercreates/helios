#pragma once

#include <string>
#include <netinet/in.h>
#include <cstdint>
#include <atomic>

namespace helios {

enum class HealthState {
    Healthy,
    Unhealthy,
    Recovering
};

enum class HealthCheckResult {
    Success,
    ConnectionFailure,
    Timeout,
    ProtocolFailure
};

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
    [[nodiscard]] bool IsEligible() const noexcept { 
        return eligible_.load(std::memory_order_relaxed) && health_state_.load(std::memory_order_relaxed) == HealthState::Healthy; 
    }
    void SetEligible(bool eligible) noexcept { eligible_ = eligible; }

    [[nodiscard]] bool IsDraining() const noexcept { return draining_; }
    void SetDraining(bool draining) noexcept { draining_ = draining; }

    // Active Connection Tracking
    void IncrementActiveConnections() noexcept { ++active_connections_; }
    void DecrementActiveConnections() noexcept { if (active_connections_ > 0) --active_connections_; }
    [[nodiscard]] uint32_t GetActiveConnections() const noexcept { return active_connections_; }

    // Health Management
    void ReportHealthCheckResult(HealthCheckResult result) noexcept;
    [[nodiscard]] HealthState GetHealthState() const noexcept { return health_state_.load(std::memory_order_relaxed); }
    void SetHealthThresholds(uint32_t failure_threshold, uint32_t recovery_threshold) noexcept;
    
    // Test inspection accessors
    [[nodiscard]] uint32_t GetConsecutiveFailures() const noexcept { return consecutive_failures_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint32_t GetConsecutiveSuccesses() const noexcept { return consecutive_successes_.load(std::memory_order_relaxed); }

private:
    uint32_t id_;
    std::string host_;
    uint16_t port_;
    sockaddr_in addr_{};
    
    uint32_t weight_;
    
    std::atomic<bool> eligible_{true};
    std::atomic<bool> draining_{false};
    
    std::atomic<uint32_t> active_connections_{0};
    
    // Health State
    std::atomic<HealthState> health_state_{HealthState::Healthy};
    std::atomic<uint32_t> consecutive_failures_{0};
    std::atomic<uint32_t> consecutive_successes_{0};
    
    uint32_t failure_threshold_{3};
    uint32_t recovery_threshold_{3};
};

} // namespace helios
