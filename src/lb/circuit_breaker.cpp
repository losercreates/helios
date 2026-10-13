#include "circuit_breaker.hpp"
#include <utility>

namespace helios {

CircuitBreaker::CircuitBreaker()
    : CircuitBreaker(CircuitBreakerConfig{}) {}

CircuitBreaker::CircuitBreaker(const CircuitBreakerConfig& config)
    : config_(config),
      state_(CircuitState::Closed),
      consecutive_failures_(0),
      probe_successes_(0),
      active_probes_(0),
      cooldown_end_(TimePoint{}) {}

CircuitBreaker::CircuitBreaker(CircuitBreaker&& other) noexcept
    : config_(other.config_),
      state_(other.state_.load(std::memory_order_relaxed)),
      consecutive_failures_(other.consecutive_failures_.load(std::memory_order_relaxed)),
      probe_successes_(other.probe_successes_.load(std::memory_order_relaxed)),
      active_probes_(other.active_probes_.load(std::memory_order_relaxed)),
      cooldown_end_(other.cooldown_end_.load(std::memory_order_relaxed)),
      time_provider_(std::move(other.time_provider_)) {}

CircuitBreaker& CircuitBreaker::operator=(CircuitBreaker&& other) noexcept {
    if (this != &other) {
        config_ = other.config_;
        state_.store(other.state_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        consecutive_failures_.store(other.consecutive_failures_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        probe_successes_.store(other.probe_successes_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        active_probes_.store(other.active_probes_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        cooldown_end_.store(other.cooldown_end_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        time_provider_ = std::move(other.time_provider_);
    }
    return *this;
}

void CircuitBreaker::CheckCooldownTransition(TimePoint now) const noexcept {
    if (state_.load(std::memory_order_relaxed) == CircuitState::Open) {
        if (now >= cooldown_end_.load(std::memory_order_relaxed)) {
            CircuitState expected = CircuitState::Open;
            if (state_.compare_exchange_strong(expected, CircuitState::HalfOpen,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
                active_probes_.store(0, std::memory_order_relaxed);
                probe_successes_.store(0, std::memory_order_relaxed);
            }
        }
    }
}

CircuitState CircuitBreaker::GetState() const noexcept {
    CheckCooldownTransition(Now());
    return state_.load(std::memory_order_relaxed);
}

bool CircuitBreaker::CanAttemptRequest() const noexcept {
    CheckCooldownTransition(Now());
    CircuitState current_state = state_.load(std::memory_order_relaxed);
    if (current_state == CircuitState::Closed) {
        return true;
    }
    if (current_state == CircuitState::Open) {
        return false;
    }
    // HalfOpen: Probe allowed only if probe capacity has not been exhausted
    return active_probes_.load(std::memory_order_relaxed) < config_.half_open_max_probes;
}

bool CircuitBreaker::AllowRequest() noexcept {
    CheckCooldownTransition(Now());
    CircuitState current_state = state_.load(std::memory_order_relaxed);
    if (current_state == CircuitState::Closed) {
        return true;
    }
    if (current_state == CircuitState::Open) {
        return false;
    }
    // HalfOpen: Atomically acquire a probe attempt
    uint32_t current_probes = active_probes_.load(std::memory_order_relaxed);
    while (current_probes < config_.half_open_max_probes) {
        if (active_probes_.compare_exchange_weak(current_probes, current_probes + 1,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void CircuitBreaker::ReportSuccess() noexcept {
    CircuitState current_state = state_.load(std::memory_order_relaxed);
    if (current_state == CircuitState::Closed) {
        consecutive_failures_.store(0, std::memory_order_relaxed);
    } else if (current_state == CircuitState::HalfOpen) {
        // Decrement active probes safely
        uint32_t current_active = active_probes_.load(std::memory_order_relaxed);
        while (current_active > 0) {
            if (active_probes_.compare_exchange_weak(current_active, current_active - 1,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
                break;
            }
        }

        uint32_t successes = probe_successes_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (successes >= config_.success_threshold) {
            CircuitState expected = CircuitState::HalfOpen;
            if (state_.compare_exchange_strong(expected, CircuitState::Closed,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
                consecutive_failures_.store(0, std::memory_order_relaxed);
                probe_successes_.store(0, std::memory_order_relaxed);
                active_probes_.store(0, std::memory_order_relaxed);
            }
        }
    }
}

void CircuitBreaker::ReportFailure() noexcept {
    CircuitState current_state = state_.load(std::memory_order_relaxed);
    if (current_state == CircuitState::Closed) {
        uint32_t failures = consecutive_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failures >= config_.failure_threshold) {
            CircuitState expected = CircuitState::Closed;
            if (state_.compare_exchange_strong(expected, CircuitState::Open,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed)) {
                cooldown_end_.store(Now() + config_.cooldown, std::memory_order_relaxed);
                active_probes_.store(0, std::memory_order_relaxed);
                probe_successes_.store(0, std::memory_order_relaxed);
            }
        }
    } else if (current_state == CircuitState::HalfOpen) {
        // Probe failed -> trip back to Open
        CircuitState expected = CircuitState::HalfOpen;
        if (state_.compare_exchange_strong(expected, CircuitState::Open,
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
            cooldown_end_.store(Now() + config_.cooldown, std::memory_order_relaxed);
            active_probes_.store(0, std::memory_order_relaxed);
            probe_successes_.store(0, std::memory_order_relaxed);
            consecutive_failures_.store(config_.failure_threshold, std::memory_order_relaxed);
        }
    } else if (current_state == CircuitState::Open) {
        // Already open -> refresh cooldown
        cooldown_end_.store(Now() + config_.cooldown, std::memory_order_relaxed);
    }
}

void CircuitBreaker::Reset() noexcept {
    state_.store(CircuitState::Closed, std::memory_order_release);
    consecutive_failures_.store(0, std::memory_order_relaxed);
    probe_successes_.store(0, std::memory_order_relaxed);
    active_probes_.store(0, std::memory_order_relaxed);
}

void CircuitBreaker::Trip() noexcept {
    state_.store(CircuitState::Open, std::memory_order_release);
    cooldown_end_.store(Now() + config_.cooldown, std::memory_order_relaxed);
    consecutive_failures_.store(config_.failure_threshold, std::memory_order_relaxed);
    active_probes_.store(0, std::memory_order_relaxed);
    probe_successes_.store(0, std::memory_order_relaxed);
}

void CircuitBreaker::SetConfig(const CircuitBreakerConfig& config) noexcept {
    config_ = config;
}

} // namespace helios
