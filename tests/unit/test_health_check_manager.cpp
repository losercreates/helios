#include <gtest/gtest.h>
#include "lb/health_check_manager.hpp"
#include "../common/test_tcp_backend.hpp"
#include <chrono>
#include <thread>
#include <memory>

using namespace helios;
using namespace std::chrono_literals;

namespace {

template <typename Predicate>
bool WaitFor(Predicate&& pred, std::chrono::milliseconds timeout = 2000ms, std::chrono::milliseconds step = 5ms) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (pred()) return true;
        std::this_thread::sleep_for(step);
    }
    return pred();
}

} // namespace

TEST(HealthCheckManagerTest, LifecycleAndRegistration) {
    HealthCheckConfig cfg;
    cfg.enabled = true;
    cfg.interval = 100ms;
    cfg.timeout = 50ms;
    cfg.unhealthy_threshold = 2;
    cfg.healthy_threshold = 1;

    HealthCheckManager manager(cfg);
    EXPECT_FALSE(manager.IsRunning());
    EXPECT_EQ(manager.GetActiveCheckerCount(), 0);

    auto backend1 = std::make_shared<Backend>(1, "127.0.0.1", 9991, 1);
    auto backend2 = std::make_shared<Backend>(2, "127.0.0.1", 9992, 1);

    manager.RegisterBackend(backend1);
    manager.RegisterBackend(backend2);

    EXPECT_TRUE(manager.Start());
    EXPECT_TRUE(manager.IsRunning());
    EXPECT_EQ(manager.GetActiveCheckerCount(), 2);

    manager.UnregisterBackend(1);
    EXPECT_TRUE(WaitFor([&]() { return manager.GetActiveCheckerCount() == 1; }));

    manager.Stop();
    EXPECT_FALSE(manager.IsRunning());
}

TEST(HealthCheckManagerTest, ActiveProbingTransitionsState) {
    // 1. Start real TCP mock backend on 127.0.0.1
    TestTcpBackend server;
    ASSERT_TRUE(server.Start());
    uint16_t server_port = server.GetPort();

    HealthCheckConfig cfg;
    cfg.enabled = true;
    cfg.interval = 30ms;
    cfg.timeout = 30ms;
    cfg.unhealthy_threshold = 2;
    cfg.healthy_threshold = 1;

    HealthCheckManager manager(cfg);
    auto backend = std::make_shared<Backend>(10, "127.0.0.1", server_port, 1);
    manager.RegisterBackend(backend);

    ASSERT_TRUE(manager.Start());

    // Initially healthy and stays healthy
    EXPECT_TRUE(backend->IsEligible());
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(backend->GetHealthState(), HealthState::Healthy);
    EXPECT_TRUE(backend->IsEligible());

    // 2. Shut down the backend -> consecutive failures accumulate -> transitions to Unhealthy
    server.Stop();

    EXPECT_TRUE(WaitFor([&]() {
        return backend->GetHealthState() == HealthState::Unhealthy;
    }, 2000ms));

    EXPECT_FALSE(backend->IsEligible());
    EXPECT_FALSE(backend->CanAcceptNewTraffic());

    // 3. Restart the backend -> probe succeeds -> transitions back to Healthy
    ASSERT_TRUE(server.Start());
    // Note: ephemeral port might change on re-bind, so update backend pre-parsed sockaddr
    backend = std::make_shared<Backend>(10, "127.0.0.1", server.GetPort(), 1);
    manager.UpdateBackends({backend});

    EXPECT_TRUE(WaitFor([&]() {
        return backend->GetHealthState() == HealthState::Healthy;
    }, 2000ms));

    EXPECT_TRUE(backend->IsEligible());
    manager.Stop();
}

TEST(HealthCheckManagerTest, UpdateBackendsReplacesCheckers) {
    HealthCheckConfig cfg;
    cfg.enabled = true;
    cfg.interval = 50ms;
    cfg.timeout = 50ms;

    HealthCheckManager manager(cfg);
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 9001, 1);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", 9002, 1);
    manager.RegisterBackend(b1);
    manager.RegisterBackend(b2);

    ASSERT_TRUE(manager.Start());
    EXPECT_EQ(manager.GetActiveCheckerCount(), 2);

    // Replace b1, b2 with b3
    auto b3 = std::make_shared<Backend>(3, "127.0.0.1", 9003, 1);
    manager.UpdateBackends({b3});

    EXPECT_TRUE(WaitFor([&]() { return manager.GetActiveCheckerCount() == 1; }));

    manager.Stop();
}

