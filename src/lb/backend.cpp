#include "backend.hpp"
#include "net/socket_utils.hpp"
#include <utility>

namespace helios {

Backend::Backend(uint32_t id, std::string host, uint16_t port, uint32_t weight)
    : id_(id), host_(std::move(host)), port_(port), weight_(weight) {
    // Pre-parse the sockaddr_in for fast hot-path usage.
    // If parsing fails, we zero it out.
    if (!SocketUtils::ParseSockAddr(host_.c_str(), port_, &addr_)) {
        addr_ = {};
    }
}

void Backend::SetHealthThresholds(uint32_t failure_threshold, uint32_t recovery_threshold) noexcept {
    failure_threshold_ = failure_threshold;
    recovery_threshold_ = recovery_threshold;
}

void Backend::ReportHealthCheckResult(HealthCheckResult result) noexcept {
    HealthState current_state = health_state_.load(std::memory_order_relaxed);

    if (result == HealthCheckResult::Success) {
        consecutive_failures_.store(0, std::memory_order_relaxed);
        
        if (current_state == HealthState::Unhealthy) {
            health_state_.store(HealthState::Recovering, std::memory_order_relaxed);
            current_state = HealthState::Recovering;
        }
        
        if (current_state == HealthState::Recovering) {
            uint32_t successes = consecutive_successes_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (successes >= recovery_threshold_) {
                health_state_.store(HealthState::Healthy, std::memory_order_relaxed);
                consecutive_successes_.store(0, std::memory_order_relaxed);
            }
        }
    } else {
        consecutive_successes_.store(0, std::memory_order_relaxed);
        
        if (current_state == HealthState::Recovering) {
            health_state_.store(HealthState::Unhealthy, std::memory_order_relaxed);
            consecutive_failures_.store(1, std::memory_order_relaxed);
        } else if (current_state == HealthState::Healthy) {
            uint32_t failures = consecutive_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (failures >= failure_threshold_) {
                health_state_.store(HealthState::Unhealthy, std::memory_order_relaxed);
                consecutive_failures_.store(0, std::memory_order_relaxed);
            }
        }
    }
}

} // namespace helios
