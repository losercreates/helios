#include <gtest/gtest.h>
#include "lb/backend.hpp"

using namespace helios;

TEST(HealthManagementTest, InitialStateIsHealthy) {
    Backend b(1, "127.0.0.1", 8080);
    EXPECT_EQ(b.GetHealthState(), HealthState::Healthy);
    EXPECT_EQ(b.GetConsecutiveFailures(), 0u);
    EXPECT_EQ(b.GetConsecutiveSuccesses(), 0u);
    EXPECT_TRUE(b.IsEligible()); // Should be eligible by default if healthy
}

TEST(HealthManagementTest, FailureThresholdTransitionsToUnhealthy) {
    Backend b(1, "127.0.0.1", 8080);
    b.SetHealthThresholds(3, 2);

    b.ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    EXPECT_EQ(b.GetHealthState(), HealthState::Healthy);
    EXPECT_EQ(b.GetConsecutiveFailures(), 1u);
    EXPECT_TRUE(b.IsEligible());

    b.ReportHealthCheckResult(HealthCheckResult::Timeout);
    EXPECT_EQ(b.GetHealthState(), HealthState::Healthy);
    EXPECT_EQ(b.GetConsecutiveFailures(), 2u);
    EXPECT_TRUE(b.IsEligible());

    // 3rd failure should transition to Unhealthy
    b.ReportHealthCheckResult(HealthCheckResult::ProtocolFailure);
    EXPECT_EQ(b.GetHealthState(), HealthState::Unhealthy);
    EXPECT_EQ(b.GetConsecutiveFailures(), 0u); // resets
    EXPECT_FALSE(b.IsEligible()); // Unhealthy backends are ineligible
}

TEST(HealthManagementTest, SuccessResetsFailureCounter) {
    Backend b(1, "127.0.0.1", 8080);
    b.SetHealthThresholds(3, 2);

    b.ReportHealthCheckResult(HealthCheckResult::Timeout);
    b.ReportHealthCheckResult(HealthCheckResult::Timeout);
    EXPECT_EQ(b.GetConsecutiveFailures(), 2u);

    // A success resets failures
    b.ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(b.GetHealthState(), HealthState::Healthy);
    EXPECT_EQ(b.GetConsecutiveFailures(), 0u);
}

TEST(HealthManagementTest, RecoveryTransitionsToHealthy) {
    Backend b(1, "127.0.0.1", 8080);
    b.SetHealthThresholds(2, 3);

    b.ReportHealthCheckResult(HealthCheckResult::Timeout);
    b.ReportHealthCheckResult(HealthCheckResult::Timeout);
    EXPECT_EQ(b.GetHealthState(), HealthState::Unhealthy);

    // 1st success moves to Recovering
    b.ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(b.GetHealthState(), HealthState::Recovering);
    EXPECT_EQ(b.GetConsecutiveSuccesses(), 1u);
    EXPECT_FALSE(b.IsEligible()); // Still ineligible while recovering

    // 2nd success
    b.ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(b.GetHealthState(), HealthState::Recovering);
    EXPECT_EQ(b.GetConsecutiveSuccesses(), 2u);
    EXPECT_FALSE(b.IsEligible());

    // 3rd success transitions to Healthy
    b.ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(b.GetHealthState(), HealthState::Healthy);
    EXPECT_EQ(b.GetConsecutiveSuccesses(), 0u);
    EXPECT_TRUE(b.IsEligible()); // Eligible again
}

TEST(HealthManagementTest, FailureDuringRecoveryRevertsToUnhealthy) {
    Backend b(1, "127.0.0.1", 8080);
    b.SetHealthThresholds(2, 3);

    b.ReportHealthCheckResult(HealthCheckResult::Timeout);
    b.ReportHealthCheckResult(HealthCheckResult::Timeout);
    EXPECT_EQ(b.GetHealthState(), HealthState::Unhealthy);

    b.ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(b.GetHealthState(), HealthState::Recovering);

    // A failure during recovery immediately drops back to Unhealthy
    b.ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    EXPECT_EQ(b.GetHealthState(), HealthState::Unhealthy);
    EXPECT_EQ(b.GetConsecutiveSuccesses(), 0u);
    EXPECT_EQ(b.GetConsecutiveFailures(), 1u);
}

TEST(HealthManagementTest, DrainingAndHealthDistinct) {
    Backend b(1, "127.0.0.1", 8080);
    b.SetDraining(true);

    EXPECT_TRUE(b.IsDraining());
    EXPECT_EQ(b.GetHealthState(), HealthState::Healthy);

    // Still eligible from a health perspective, but lb skips draining nodes.
    // However, IsEligible() technically checks eligible_ && health_state_.
    EXPECT_TRUE(b.IsEligible()); 
    
    // Fail it
    b.SetHealthThresholds(1, 1);
    b.ReportHealthCheckResult(HealthCheckResult::Timeout);
    EXPECT_EQ(b.GetHealthState(), HealthState::Unhealthy);
    
    // Draining should remain true
    EXPECT_TRUE(b.IsDraining());
    EXPECT_FALSE(b.IsEligible());
}

TEST(HealthManagementTest, RepeatedFailuresDontOverflow) {
    Backend b(1, "127.0.0.1", 8080);
    b.SetHealthThresholds(2, 2);

    b.ReportHealthCheckResult(HealthCheckResult::Timeout);
    b.ReportHealthCheckResult(HealthCheckResult::Timeout);
    EXPECT_EQ(b.GetHealthState(), HealthState::Unhealthy);
    
    for (int i = 0; i < 100; ++i) {
        b.ReportHealthCheckResult(HealthCheckResult::Timeout);
    }
    
    EXPECT_EQ(b.GetHealthState(), HealthState::Unhealthy);
    EXPECT_EQ(b.GetConsecutiveSuccesses(), 0u);
}

TEST(HealthManagementTest, RepeatedRecoveriesDontOverflow) {
    Backend b(1, "127.0.0.1", 8080);
    b.SetHealthThresholds(2, 2);

    for (int i = 0; i < 100; ++i) {
        b.ReportHealthCheckResult(HealthCheckResult::Success);
    }
    
    EXPECT_EQ(b.GetHealthState(), HealthState::Healthy);
    EXPECT_EQ(b.GetConsecutiveFailures(), 0u);
    EXPECT_EQ(b.GetConsecutiveSuccesses(), 0u);
}
