#pragma once

#include <cstdint>
#include <chrono>
#include <atomic>
#include <functional>

namespace helios {

enum class CircuitState {
    Closed,
    Open,
    HalfOpen
};

struct CircuitBreakerConfig {
    uint32_t failure_threshold{3};
    uint32_t success_threshold{1};
    std::chrono::milliseconds cooldown{5000};
    uint32_t half_open_max_probes{1};
};

// Represents an asynchronous, lock-free backend Circuit Breaker.
// Tracks consecutive connection/transport failures to protect failing backends from traffic storms.
// Adheres to standard states: Closed (normal), Open (tripped), HalfOpen (probing).
class CircuitBreaker {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using TimeProvider = std::function<TimePoint()>;

    CircuitBreaker();
    explicit CircuitBreaker(const CircuitBreakerConfig& config);
    ~CircuitBreaker() = default;

    // Movable, non-copyable due to atomics
    CircuitBreaker(const CircuitBreaker&) = delete;
    CircuitBreaker& operator=(const CircuitBreaker&) = delete;
    CircuitBreaker(CircuitBreaker&& other) noexcept;
    CircuitBreaker& operator=(CircuitBreaker&& other) noexcept;

    // Current circuit state evaluation
    [[nodiscard]] CircuitState GetState() const noexcept;

    // Non-mutating probe/admission check used for backend selection eligibility
    [[nodiscard]] bool CanAttemptRequest() const noexcept;

    // Atomically acquires permission to send a request:
    // - Closed: returns true
    // - Open: evaluates cooldown. If elapsed, promotes to HalfOpen; else returns false
    // - HalfOpen: atomically reserves 1 probe slot up to half_open_max_probes
    [[nodiscard]] bool AllowRequest() noexcept;

    // Reports the outcome of a transport connection attempt
    void ReportSuccess() noexcept;
    void ReportFailure() noexcept;

    // Manual lifecycle controls
    void Reset() noexcept;
    void Trip() noexcept;

    // Configuration
    void SetConfig(const CircuitBreakerConfig& config) noexcept;
    [[nodiscard]] const CircuitBreakerConfig& GetConfig() const noexcept { return config_; }
    void SetFailureThreshold(uint32_t threshold) noexcept { config_.failure_threshold = threshold; }
    void SetSuccessThreshold(uint32_t threshold) noexcept { config_.success_threshold = threshold; }
    void SetCooldown(std::chrono::milliseconds cooldown) noexcept { config_.cooldown = cooldown; }
    void SetHalfOpenMaxProbes(uint32_t max_probes) noexcept { config_.half_open_max_probes = max_probes; }

    // Metrics & inspection
    [[nodiscard]] uint32_t GetConsecutiveFailures() const noexcept { return consecutive_failures_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint32_t GetActiveProbes() const noexcept { return active_probes_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint32_t GetProbeSuccesses() const noexcept { return probe_successes_.load(std::memory_order_relaxed); }
    [[nodiscard]] TimePoint GetCooldownEnd() const noexcept { return cooldown_end_.load(std::memory_order_relaxed); }

    // Injectable time provider for deterministic testing with zero sleeping
    void SetTimeProvider(TimeProvider provider) noexcept { time_provider_ = std::move(provider); }

private:
    [[nodiscard]] TimePoint Now() const noexcept {
        if (time_provider_) {
            return time_provider_();
        }
        return std::chrono::steady_clock::now();
    }

    void CheckCooldownTransition(TimePoint now) const noexcept;

    CircuitBreakerConfig config_;
    mutable std::atomic<CircuitState> state_{CircuitState::Closed};
    std::atomic<uint32_t> consecutive_failures_{0};
    mutable std::atomic<uint32_t> probe_successes_{0};
    mutable std::atomic<uint32_t> active_probes_{0};
    mutable std::atomic<TimePoint> cooldown_end_{TimePoint{}};

    TimeProvider time_provider_{nullptr};
};

} // namespace helios
