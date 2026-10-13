#include <gtest/gtest.h>
#include "lb/backend.hpp"
#include "lb/circuit_breaker.hpp"
#include "lb/round_robin.hpp"
#include "lb/weighted_round_robin.hpp"
#include "lb/least_connections.hpp"
#include "lb/consistent_hash.hpp"

#include <memory>
#include <vector>

using namespace helios;

namespace {

// Helper to test selection across all 4 load balancers with a single backend
struct AllBalancersSingle {
    RoundRobinLoadBalancer rr;
    WeightedRoundRobinLoadBalancer wrr;
    LeastConnectionsLoadBalancer lc;
    ConsistentHashLoadBalancer ch;

    explicit AllBalancersSingle(std::shared_ptr<Backend> b) {
        rr.AddBackend(b);
        wrr.AddBackend(b);
        lc.AddBackend(b);
        ch.AddBackend(b);
    }

    void AssertSelected(uint32_t expected_id) {
        auto sel_rr = rr.SelectBackend();
        ASSERT_NE(sel_rr, nullptr);
        EXPECT_EQ(sel_rr->GetId(), expected_id);

        auto sel_wrr = wrr.SelectBackend();
        ASSERT_NE(sel_wrr, nullptr);
        EXPECT_EQ(sel_wrr->GetId(), expected_id);

        auto sel_lc = lc.SelectBackend();
        ASSERT_NE(sel_lc, nullptr);
        EXPECT_EQ(sel_lc->GetId(), expected_id);

        auto sel_ch = ch.SelectBackend();
        ASSERT_NE(sel_ch, nullptr);
        EXPECT_EQ(sel_ch->GetId(), expected_id);
    }

    void AssertNoneSelected() {
        EXPECT_EQ(rr.SelectBackend(), nullptr);
        EXPECT_EQ(wrr.SelectBackend(), nullptr);
        EXPECT_EQ(lc.SelectBackend(), nullptr);
        EXPECT_EQ(ch.SelectBackend(), nullptr);
    }
};

} // namespace

// 1. healthy + active
TEST(BackendSelectionMatrixTest, Combination1_HealthyAndActive) {
    auto b = std::make_shared<Backend>(1, "127.0.0.1", 8080);
    EXPECT_TRUE(b->IsAdministrativelyEligible());
    EXPECT_FALSE(b->IsDraining());
    EXPECT_EQ(b->GetHealthState(), HealthState::Healthy);
    EXPECT_EQ(b->GetCircuitState(), CircuitState::Closed);

    EXPECT_TRUE(b->CanAcceptNewTraffic());

    AllBalancersSingle lbs(b);
    lbs.AssertSelected(1);
}

// 2. healthy + draining
TEST(BackendSelectionMatrixTest, Combination2_HealthyAndDraining) {
    auto b = std::make_shared<Backend>(1, "127.0.0.1", 8080);
    b->SetDraining(true);

    EXPECT_TRUE(b->IsAdministrativelyEligible());
    EXPECT_TRUE(b->IsDraining());
    EXPECT_EQ(b->GetHealthState(), HealthState::Healthy);
    EXPECT_EQ(b->GetCircuitState(), CircuitState::Closed);

    // Administrative draining overrides health and circuit state
    EXPECT_FALSE(b->CanAcceptNewTraffic());

    AllBalancersSingle lbs(b);
    lbs.AssertNoneSelected();
}

// 3. unhealthy + active
TEST(BackendSelectionMatrixTest, Combination3_UnhealthyAndActive) {
    auto b = std::make_shared<Backend>(1, "127.0.0.1", 8080);
    b->SetHealthThresholds(1, 1);
    b->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);

    EXPECT_TRUE(b->IsAdministrativelyEligible());
    EXPECT_FALSE(b->IsDraining());
    EXPECT_EQ(b->GetHealthState(), HealthState::Unhealthy);
    EXPECT_EQ(b->GetCircuitState(), CircuitState::Closed);

    // Unhealthy state blocks new traffic
    EXPECT_FALSE(b->CanAcceptNewTraffic());

    AllBalancersSingle lbs(b);
    lbs.AssertNoneSelected();
}

// 4. unhealthy + draining
TEST(BackendSelectionMatrixTest, Combination4_UnhealthyAndDraining) {
    auto b = std::make_shared<Backend>(1, "127.0.0.1", 8080);
    b->SetDraining(true);
    b->SetHealthThresholds(1, 1);
    b->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);

    EXPECT_TRUE(b->IsDraining());
    EXPECT_EQ(b->GetHealthState(), HealthState::Unhealthy);

    // Both draining and unhealthy block selection
    EXPECT_FALSE(b->CanAcceptNewTraffic());

    AllBalancersSingle lbs(b);
    lbs.AssertNoneSelected();
}

// 5. circuit CLOSED
TEST(BackendSelectionMatrixTest, Combination5_CircuitClosed) {
    auto b = std::make_shared<Backend>(1, "127.0.0.1", 8080);
    EXPECT_EQ(b->GetCircuitState(), CircuitState::Closed);
    EXPECT_TRUE(b->CanAcceptNewTraffic());

    AllBalancersSingle lbs(b);
    lbs.AssertSelected(1);
}

// 6. circuit OPEN
TEST(BackendSelectionMatrixTest, Combination6_CircuitOpen) {
    auto b = std::make_shared<Backend>(1, "127.0.0.1", 8080);
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = std::chrono::milliseconds(10000);
    b->ConfigureCircuitBreaker(cfg);

    b->ReportConnectionFailure();
    EXPECT_EQ(b->GetCircuitState(), CircuitState::Open);

    // Circuit Open blocks traffic even when healthy, active, and not draining
    EXPECT_FALSE(b->CanAcceptNewTraffic());

    AllBalancersSingle lbs(b);
    lbs.AssertNoneSelected();
}

// 7. circuit HALF_OPEN (probe slot available vs probe slot exhausted)
TEST(BackendSelectionMatrixTest, Combination7_CircuitHalfOpen) {
    auto b = std::make_shared<Backend>(1, "127.0.0.1", 8080);
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = std::chrono::milliseconds(10);
    cfg.half_open_max_probes = 1;
    b->ConfigureCircuitBreaker(cfg);

    // Trip to OPEN
    b->ReportConnectionFailure();
    EXPECT_EQ(b->GetCircuitState(), CircuitState::Open);

    // Cooldown expires -> transitions to HALF_OPEN
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    EXPECT_EQ(b->GetCircuitState(), CircuitState::HalfOpen);

    // Probe slot available (0 / 1 active probes)
    EXPECT_TRUE(b->CanAcceptNewTraffic());

    // Reserve probe
    EXPECT_TRUE(b->GetCircuitBreaker().AllowRequest());
    EXPECT_EQ(b->GetCircuitBreaker().GetActiveProbes(), 1u);

    // Now probe slot is exhausted (1 / 1 active probes)
    EXPECT_FALSE(b->CanAcceptNewTraffic());

    AllBalancersSingle lbs(b);
    lbs.AssertNoneSelected();

    // Successful probe returns circuit to CLOSED
    b->ReportConnectionSuccess();
    EXPECT_EQ(b->GetCircuitState(), CircuitState::Closed);
    EXPECT_TRUE(b->CanAcceptNewTraffic());
    lbs.AssertSelected(1);
}

// 8. zero eligible backends
TEST(BackendSelectionMatrixTest, Combination8_ZeroEligibleBackends) {
    // Empty load balancers
    RoundRobinLoadBalancer rr;
    WeightedRoundRobinLoadBalancer wrr;
    LeastConnectionsLoadBalancer lc;
    ConsistentHashLoadBalancer ch;

    EXPECT_EQ(rr.SelectBackend(), nullptr);
    EXPECT_EQ(wrr.SelectBackend(), nullptr);
    EXPECT_EQ(lc.SelectBackend(), nullptr);
    EXPECT_EQ(ch.SelectBackend(), nullptr);

    // Pool of 3 backends where NONE are eligible
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 8081);
    b1->SetDraining(true); // Draining

    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 8082);
    b2->SetHealthThresholds(1, 1);
    b2->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure); // Unhealthy

    auto b3 = std::make_shared<Backend>(3, "127.0.0.1", 8083);
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = std::chrono::milliseconds(10000);
    b3->ConfigureCircuitBreaker(cfg);
    b3->ReportConnectionFailure(); // Circuit Open

    for (auto* lb : std::vector<LoadBalancer*>{&rr, &wrr, &lc, &ch}) {
        lb->AddBackend(b1);
        lb->AddBackend(b2);
        lb->AddBackend(b3);
        EXPECT_EQ(lb->SelectBackend(), nullptr);
    }
}

// 9. one eligible backend out of many
TEST(BackendSelectionMatrixTest, Combination9_OneEligibleBackend) {
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 8081);
    b1->SetDraining(true); // Ineligible: draining

    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 8082);
    b2->SetHealthThresholds(1, 1);
    b2->ReportHealthCheckResult(HealthCheckResult::Timeout); // Ineligible: unhealthy

    auto b3 = std::make_shared<Backend>(3, "127.0.0.1", 8083);
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = std::chrono::milliseconds(10000);
    b3->ConfigureCircuitBreaker(cfg);
    b3->ReportConnectionFailure(); // Ineligible: circuit open

    auto b4 = std::make_shared<Backend>(4, "127.0.0.1", 8084); // The ONLY eligible backend

    RoundRobinLoadBalancer rr;
    WeightedRoundRobinLoadBalancer wrr;
    LeastConnectionsLoadBalancer lc;
    ConsistentHashLoadBalancer ch;

    for (auto* lb : std::vector<LoadBalancer*>{&rr, &wrr, &lc, &ch}) {
        lb->AddBackend(b1);
        lb->AddBackend(b2);
        lb->AddBackend(b3);
        lb->AddBackend(b4);
        
        for (int i = 0; i < 5; ++i) {
            auto sel = lb->SelectBackend();
            ASSERT_NE(sel, nullptr);
            EXPECT_EQ(sel->GetId(), 4u);
        }
    }
}

// 10. all backends unhealthy
TEST(BackendSelectionMatrixTest, Combination10_AllBackendsUnhealthy) {
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 8081);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 8082);
    auto b3 = std::make_shared<Backend>(3, "127.0.0.1", 8083);

    b1->SetHealthThresholds(1, 1);
    b2->SetHealthThresholds(1, 1);
    b3->SetHealthThresholds(1, 1);

    b1->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    b2->ReportHealthCheckResult(HealthCheckResult::Timeout);
    b3->ReportHealthCheckResult(HealthCheckResult::ProtocolFailure);

    RoundRobinLoadBalancer rr;
    WeightedRoundRobinLoadBalancer wrr;
    LeastConnectionsLoadBalancer lc;
    ConsistentHashLoadBalancer ch;

    for (auto* lb : std::vector<LoadBalancer*>{&rr, &wrr, &lc, &ch}) {
        lb->AddBackend(b1);
        lb->AddBackend(b2);
        lb->AddBackend(b3);
        EXPECT_EQ(lb->SelectBackend(), nullptr);
    }
}

// 11. all backends draining
TEST(BackendSelectionMatrixTest, Combination11_AllBackendsDraining) {
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 8081);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 8082);
    auto b3 = std::make_shared<Backend>(3, "127.0.0.1", 8083);

    b1->SetDraining(true);
    b2->SetDraining(true);
    b3->SetDraining(true);

    RoundRobinLoadBalancer rr;
    WeightedRoundRobinLoadBalancer wrr;
    LeastConnectionsLoadBalancer lc;
    ConsistentHashLoadBalancer ch;

    for (auto* lb : std::vector<LoadBalancer*>{&rr, &wrr, &lc, &ch}) {
        lb->AddBackend(b1);
        lb->AddBackend(b2);
        lb->AddBackend(b3);
        EXPECT_EQ(lb->SelectBackend(), nullptr);
    }
}

// 12. backend recovering
TEST(BackendSelectionMatrixTest, Combination12_BackendRecovering) {
    auto b = std::make_shared<Backend>(1, "127.0.0.1", 8080);
    b->SetHealthThresholds(1, 3); // Requires 3 successes to recover

    // Mark unhealthy
    b->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    EXPECT_EQ(b->GetHealthState(), HealthState::Unhealthy);
    EXPECT_FALSE(b->CanAcceptNewTraffic());

    // 1st success -> Recovering (not yet Healthy)
    b->ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(b->GetHealthState(), HealthState::Recovering);
    EXPECT_FALSE(b->CanAcceptNewTraffic());

    AllBalancersSingle lbs(b);
    lbs.AssertNoneSelected();

    // 2nd success -> Still Recovering
    b->ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(b->GetHealthState(), HealthState::Recovering);
    EXPECT_FALSE(b->CanAcceptNewTraffic());
    lbs.AssertNoneSelected();

    // 3rd success -> Transitions to Healthy
    b->ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(b->GetHealthState(), HealthState::Healthy);
    EXPECT_TRUE(b->CanAcceptNewTraffic());
    lbs.AssertSelected(1);
}

// Administrative disabled precedence
TEST(BackendSelectionMatrixTest, PrecedenceRule_AdministrativeDisabled) {
    auto b = std::make_shared<Backend>(1, "127.0.0.1", 8080);
    b->SetEligible(false); // Administratively disabled

    EXPECT_FALSE(b->IsAdministrativelyEligible());
    EXPECT_EQ(b->GetHealthState(), HealthState::Healthy);
    EXPECT_EQ(b->GetCircuitState(), CircuitState::Closed);
    EXPECT_FALSE(b->IsDraining());

    // Administrative disabled blocks selection
    EXPECT_FALSE(b->CanAcceptNewTraffic());

    AllBalancersSingle lbs(b);
    lbs.AssertNoneSelected();

    b->SetEligible(true);
    EXPECT_TRUE(b->CanAcceptNewTraffic());
    lbs.AssertSelected(1);
}

// Precedence Hierarchy:
// Tier 1: Administrative Draining
// Tier 2: Administrative Eligibility
// Tier 3: Health State
// Tier 4: Circuit Breaker
TEST(BackendSelectionMatrixTest, DeterministicPrecedenceHierarchy) {
    auto b = std::make_shared<Backend>(1, "127.0.0.1", 8080);
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = std::chrono::milliseconds(10000);
    b->ConfigureCircuitBreaker(cfg);
    b->SetHealthThresholds(1, 1);

    // Base state: eligible
    EXPECT_TRUE(b->CanAcceptNewTraffic());

    // Tier 4: Circuit Open blocks
    b->ReportConnectionFailure();
    EXPECT_EQ(b->GetCircuitState(), CircuitState::Open);
    EXPECT_FALSE(b->CanAcceptNewTraffic());

    // Reset circuit
    b->GetCircuitBreaker().Reset();
    EXPECT_TRUE(b->CanAcceptNewTraffic());

    // Tier 3: Unhealthy blocks
    b->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    EXPECT_EQ(b->GetHealthState(), HealthState::Unhealthy);
    EXPECT_FALSE(b->CanAcceptNewTraffic());

    // Restore health
    b->ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(b->GetHealthState(), HealthState::Healthy);
    EXPECT_TRUE(b->CanAcceptNewTraffic());

    // Tier 2: Admin disabled blocks
    b->SetEligible(false);
    EXPECT_FALSE(b->CanAcceptNewTraffic());

    // Re-enable
    b->SetEligible(true);
    EXPECT_TRUE(b->CanAcceptNewTraffic());

    // Tier 1: Draining blocks everything
    b->SetDraining(true);
    EXPECT_FALSE(b->CanAcceptNewTraffic());
}

// Existing connections must not be force-closed merely because state changes
TEST(BackendSelectionMatrixTest, ExistingConnectionsUnaffectedByStateChanges) {
    auto b = std::make_shared<Backend>(1, "127.0.0.1", 8080);
    b->IncrementActiveConnections();
    b->IncrementActiveConnections();
    b->IncrementActiveConnections();
    EXPECT_EQ(b->GetActiveConnections(), 3u);

    // State change: Draining
    b->SetDraining(true);
    EXPECT_EQ(b->GetActiveConnections(), 3u); // Undisturbed
    EXPECT_FALSE(b->CanAcceptNewTraffic());

    // State change: Unhealthy
    b->SetHealthThresholds(1, 1);
    b->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    EXPECT_EQ(b->GetHealthState(), HealthState::Unhealthy);
    EXPECT_EQ(b->GetActiveConnections(), 3u); // Undisturbed

    // State change: Circuit Open
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.cooldown = std::chrono::milliseconds(10000);
    b->ConfigureCircuitBreaker(cfg);
    b->ReportConnectionFailure();
    EXPECT_EQ(b->GetCircuitState(), CircuitState::Open);
    EXPECT_EQ(b->GetActiveConnections(), 3u); // Undisturbed

    // Connections safely close one by one
    b->DecrementActiveConnections();
    EXPECT_EQ(b->GetActiveConnections(), 2u);
    b->DecrementActiveConnections();
    EXPECT_EQ(b->GetActiveConnections(), 1u);
    b->DecrementActiveConnections();
    EXPECT_EQ(b->GetActiveConnections(), 0u);
}

// Exhaustive Cartesian matrix verifying all combinations
TEST(BackendSelectionMatrixTest, ExhaustiveCartesianMatrix48Combinations) {
    for (bool admin_eligible : {true, false}) {
        for (bool draining : {true, false}) {
            for (HealthState health : {HealthState::Healthy, HealthState::Unhealthy, HealthState::Recovering}) {
                for (int circuit_mode : {0, 1, 2, 3}) { // 0: Closed, 1: Open, 2: HalfOpen-free, 3: HalfOpen-saturated
                    auto b = std::make_shared<Backend>(100, "127.0.0.1", 8080);
                    b->SetEligible(admin_eligible);
                    b->SetDraining(draining);

                    b->SetHealthThresholds(1, 2);
                    if (health == HealthState::Unhealthy) {
                        b->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
                    } else if (health == HealthState::Recovering) {
                        b->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
                        b->ReportHealthCheckResult(HealthCheckResult::Success); // 1 / 2 -> Recovering
                    }

                    CircuitBreakerConfig cfg;
                    cfg.failure_threshold = 1;
                    cfg.cooldown = std::chrono::milliseconds(10);
                    cfg.half_open_max_probes = 1;
                    b->ConfigureCircuitBreaker(cfg);

                    if (circuit_mode == 1) { // Open
                        b->ReportConnectionFailure();
                    } else if (circuit_mode == 2) { // HalfOpen with free probe
                        b->ReportConnectionFailure();
                        std::this_thread::sleep_for(std::chrono::milliseconds(15));
                    } else if (circuit_mode == 3) { // HalfOpen saturated
                        b->ReportConnectionFailure();
                        std::this_thread::sleep_for(std::chrono::milliseconds(15));
                        [[maybe_unused]] bool allowed = b->GetCircuitBreaker().AllowRequest(); // Probe consumed
                    }

                    bool expected_acceptable = admin_eligible && !draining && (health == HealthState::Healthy) && (circuit_mode == 0 || circuit_mode == 2);
                    EXPECT_EQ(b->CanAcceptNewTraffic(), expected_acceptable);

                    AllBalancersSingle lbs(b);
                    if (expected_acceptable) {
                        lbs.AssertSelected(100);
                    } else {
                        lbs.AssertNoneSelected();
                    }
                }
            }
        }
    }
}
