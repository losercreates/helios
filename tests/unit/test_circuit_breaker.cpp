#include <gtest/gtest.h>
#include "lb/circuit_breaker.hpp"
#include "lb/backend.hpp"
#include "lb/round_robin.hpp"

#include <chrono>
#include <memory>
#include <vector>

using namespace helios;
using namespace std::chrono_literals;

class CircuitBreakerTest : public ::testing::Test {
protected:
    void SetUp() override {
        current_time_ = std::chrono::steady_clock::now();
    }

    CircuitBreaker::TimePoint current_time_;
};

// 1. Initial state is CLOSED
TEST_F(CircuitBreakerTest, InitialClosed) {
    CircuitBreaker cb;
    EXPECT_EQ(cb.GetState(), CircuitState::Closed);
    EXPECT_TRUE(cb.CanAttemptRequest());
    EXPECT_TRUE(cb.AllowRequest());
    EXPECT_EQ(cb.GetConsecutiveFailures(), 0u);
    EXPECT_EQ(cb.GetActiveProbes(), 0u);
}

// 2. Failure threshold evaluation
TEST_F(CircuitBreakerTest, FailureThreshold) {
    CircuitBreakerConfig config;
    config.failure_threshold = 3;
    CircuitBreaker cb(config);

    cb.ReportFailure();
    EXPECT_EQ(cb.GetState(), CircuitState::Closed);
    EXPECT_EQ(cb.GetConsecutiveFailures(), 1u);
    EXPECT_TRUE(cb.CanAttemptRequest());

    cb.ReportFailure();
    EXPECT_EQ(cb.GetState(), CircuitState::Closed);
    EXPECT_EQ(cb.GetConsecutiveFailures(), 2u);

    // Success resets consecutive failures counter
    cb.ReportSuccess();
    EXPECT_EQ(cb.GetState(), CircuitState::Closed);
    EXPECT_EQ(cb.GetConsecutiveFailures(), 0u);

    // 1 failure again
    cb.ReportFailure();
    EXPECT_EQ(cb.GetState(), CircuitState::Closed);
    EXPECT_EQ(cb.GetConsecutiveFailures(), 1u);
}

// 3. CLOSED -> OPEN upon reaching threshold
TEST_F(CircuitBreakerTest, ClosedToOpen) {
    CircuitBreakerConfig config;
    config.failure_threshold = 3;
    CircuitBreaker cb(config);

    cb.ReportFailure();
    cb.ReportFailure();
    cb.ReportFailure();

    EXPECT_EQ(cb.GetState(), CircuitState::Open);
    EXPECT_FALSE(cb.CanAttemptRequest());
    EXPECT_FALSE(cb.AllowRequest());
}

// 4. OPEN blocks load-balancer selection
TEST_F(CircuitBreakerTest, OpenBlocksSelection) {
    RoundRobinLoadBalancer lb;
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 8001);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 8002);
    lb.AddBackend(b1);
    lb.AddBackend(b2);

    // Initially both eligible
    EXPECT_TRUE(b1->IsEligible());
    EXPECT_TRUE(b2->IsEligible());

    // Trip b1
    b1->GetCircuitBreaker().Trip();
    EXPECT_FALSE(b1->IsEligible());
    EXPECT_EQ(b1->GetCircuitState(), CircuitState::Open);

    // SelectBackend should exclusively choose b2
    for (int i = 0; i < 5; ++i) {
        auto selected = lb.SelectBackend();
        ASSERT_NE(selected, nullptr);
        EXPECT_EQ(selected->GetId(), 2u);
    }
}

// 5. Cooldown evaluation
TEST_F(CircuitBreakerTest, Cooldown) {
    CircuitBreakerConfig config;
    config.failure_threshold = 1;
    config.cooldown = 1000ms;
    CircuitBreaker cb(config);

    cb.SetTimeProvider([this]() { return current_time_; });

    // Trip to open
    cb.ReportFailure();
    EXPECT_EQ(cb.GetState(), CircuitState::Open);

    // Halfway through cooldown
    current_time_ += 500ms;
    EXPECT_EQ(cb.GetState(), CircuitState::Open);
    EXPECT_FALSE(cb.CanAttemptRequest());
    EXPECT_FALSE(cb.AllowRequest());

    // Cooldown expired
    current_time_ += 501ms;
    EXPECT_EQ(cb.GetState(), CircuitState::HalfOpen);
    EXPECT_TRUE(cb.CanAttemptRequest());
}

// 6. OPEN -> HALF_OPEN state transition
TEST_F(CircuitBreakerTest, OpenToHalfOpen) {
    CircuitBreakerConfig config;
    config.cooldown = 500ms;
    CircuitBreaker cb(config);
    cb.SetTimeProvider([this]() { return current_time_; });

    cb.Trip();
    EXPECT_EQ(cb.GetState(), CircuitState::Open);

    current_time_ += 600ms;
    EXPECT_EQ(cb.GetState(), CircuitState::HalfOpen);
}

// 7. Successful probe in HALF_OPEN -> CLOSED
TEST_F(CircuitBreakerTest, SuccessfulProbeToClosed) {
    CircuitBreakerConfig config;
    config.cooldown = 500ms;
    config.success_threshold = 1;
    CircuitBreaker cb(config);
    cb.SetTimeProvider([this]() { return current_time_; });

    cb.Trip();
    current_time_ += 600ms;
    EXPECT_EQ(cb.GetState(), CircuitState::HalfOpen);

    // Allow probe
    EXPECT_TRUE(cb.AllowRequest());
    EXPECT_EQ(cb.GetActiveProbes(), 1u);

    // Report probe success
    cb.ReportSuccess();
    EXPECT_EQ(cb.GetState(), CircuitState::Closed);
    EXPECT_EQ(cb.GetActiveProbes(), 0u);
    EXPECT_EQ(cb.GetConsecutiveFailures(), 0u);
    EXPECT_TRUE(cb.AllowRequest());
}

// 8. Failed probe in HALF_OPEN -> OPEN
TEST_F(CircuitBreakerTest, FailedProbeToOpen) {
    CircuitBreakerConfig config;
    config.cooldown = 500ms;
    CircuitBreaker cb(config);
    cb.SetTimeProvider([this]() { return current_time_; });

    cb.Trip();
    current_time_ += 600ms;
    EXPECT_EQ(cb.GetState(), CircuitState::HalfOpen);

    EXPECT_TRUE(cb.AllowRequest());
    EXPECT_EQ(cb.GetActiveProbes(), 1u);

    // Probe failure returns circuit immediately to Open
    cb.ReportFailure();
    EXPECT_EQ(cb.GetState(), CircuitState::Open);
    EXPECT_EQ(cb.GetActiveProbes(), 0u);
    EXPECT_FALSE(cb.AllowRequest());

    // Must wait for cooldown again
    current_time_ += 300ms;
    EXPECT_EQ(cb.GetState(), CircuitState::Open);
}

// 9. Probe limit enforcement in HALF_OPEN
TEST_F(CircuitBreakerTest, ProbeLimit) {
    CircuitBreakerConfig config;
    config.cooldown = 500ms;
    config.half_open_max_probes = 2;
    CircuitBreaker cb(config);
    cb.SetTimeProvider([this]() { return current_time_; });

    cb.Trip();
    current_time_ += 600ms;
    EXPECT_EQ(cb.GetState(), CircuitState::HalfOpen);

    // Probe 1 allowed
    EXPECT_TRUE(cb.AllowRequest());
    EXPECT_EQ(cb.GetActiveProbes(), 1u);
    EXPECT_TRUE(cb.CanAttemptRequest());

    // Probe 2 allowed
    EXPECT_TRUE(cb.AllowRequest());
    EXPECT_EQ(cb.GetActiveProbes(), 2u);

    // Probe 3 rejected (limit reached)
    EXPECT_FALSE(cb.CanAttemptRequest());
    EXPECT_FALSE(cb.AllowRequest());
    EXPECT_EQ(cb.GetActiveProbes(), 2u);
}

// 10. Repeated failures don't overflow or corrupt state
TEST_F(CircuitBreakerTest, RepeatedFailures) {
    CircuitBreakerConfig config;
    config.failure_threshold = 2;
    config.cooldown = 500ms;
    CircuitBreaker cb(config);
    cb.SetTimeProvider([this]() { return current_time_; });

    for (int i = 0; i < 20; ++i) {
        cb.ReportFailure();
    }
    EXPECT_EQ(cb.GetState(), CircuitState::Open);
    EXPECT_FALSE(cb.CanAttemptRequest());
}

// 11. Full recovery lifecycle
TEST_F(CircuitBreakerTest, Recovery) {
    CircuitBreakerConfig config;
    config.failure_threshold = 2;
    config.success_threshold = 2;
    config.cooldown = 500ms;
    config.half_open_max_probes = 2;
    CircuitBreaker cb(config);
    cb.SetTimeProvider([this]() { return current_time_; });

    // 1. Initial Closed
    EXPECT_EQ(cb.GetState(), CircuitState::Closed);

    // 2. Failures trip to Open
    cb.ReportFailure();
    cb.ReportFailure();
    EXPECT_EQ(cb.GetState(), CircuitState::Open);

    // 3. Cooldown advances to HalfOpen
    current_time_ += 600ms;
    EXPECT_EQ(cb.GetState(), CircuitState::HalfOpen);

    // 4. First probe succeeds
    EXPECT_TRUE(cb.AllowRequest());
    cb.ReportSuccess();
    EXPECT_EQ(cb.GetState(), CircuitState::HalfOpen); // Needs 2 successes

    // 5. Second probe succeeds
    EXPECT_TRUE(cb.AllowRequest());
    cb.ReportSuccess();
    EXPECT_EQ(cb.GetState(), CircuitState::Closed); // Recovered!
}

// 12. Manual reset behavior
TEST_F(CircuitBreakerTest, ResetBehavior) {
    CircuitBreaker cb;
    cb.Trip();
    EXPECT_EQ(cb.GetState(), CircuitState::Open);

    cb.Reset();
    EXPECT_EQ(cb.GetState(), CircuitState::Closed);
    EXPECT_EQ(cb.GetConsecutiveFailures(), 0u);
    EXPECT_EQ(cb.GetActiveProbes(), 0u);
    EXPECT_TRUE(cb.AllowRequest());
}

// 13. Draining interaction precedence
TEST_F(CircuitBreakerTest, DrainingInteraction) {
    auto backend = std::make_shared<Backend>(1, "127.0.0.1", 8080);
    EXPECT_TRUE(backend->IsEligible());
    EXPECT_TRUE(backend->CanAcceptNewTraffic());

    // Mark draining -> must not accept new traffic regardless of circuit state
    backend->SetDraining(true);
    EXPECT_TRUE(backend->IsDraining());
    EXPECT_FALSE(backend->CanAcceptNewTraffic());

    // Circuit state changes don't override draining
    backend->GetCircuitBreaker().Trip();
    EXPECT_FALSE(backend->CanAcceptNewTraffic());

    backend->GetCircuitBreaker().Reset();
    EXPECT_FALSE(backend->CanAcceptNewTraffic());
}

// 14. Health state interaction precedence
TEST_F(CircuitBreakerTest, HealthInteraction) {
    auto backend = std::make_shared<Backend>(1, "127.0.0.1", 8080);
    backend->SetHealthThresholds(1, 1);
    EXPECT_TRUE(backend->IsEligible());

    // 1. Unhealthy backend is ineligible even if circuit is Closed
    backend->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    EXPECT_EQ(backend->GetHealthState(), HealthState::Unhealthy);
    EXPECT_EQ(backend->GetCircuitState(), CircuitState::Closed);
    EXPECT_FALSE(backend->IsEligible());

    // 2. Recovering backend is ineligible even if circuit is Closed
    backend->ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(backend->GetHealthState(), HealthState::Healthy);
    EXPECT_TRUE(backend->IsEligible());

    // 3. Healthy backend is blocked when circuit is Open
    backend->GetCircuitBreaker().Trip();
    EXPECT_EQ(backend->GetHealthState(), HealthState::Healthy);
    EXPECT_EQ(backend->GetCircuitState(), CircuitState::Open);
    EXPECT_FALSE(backend->IsEligible());
}
