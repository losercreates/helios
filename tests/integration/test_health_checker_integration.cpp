#include <gtest/gtest.h>
#include "lb/health_checker.hpp"
#include "lb/round_robin.hpp"
#include "io/ring_engine.hpp"
#include "net/l4_proxy_server.hpp"
#include "net/socket_utils.hpp"
#include "../common/test_tcp_backend.hpp"

#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <atomic>
#include <memory>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

using namespace helios;
using namespace std::chrono_literals;

namespace {

template <typename Predicate>
bool WaitFor(Predicate&& pred, std::chrono::milliseconds timeout = 2000ms, std::chrono::milliseconds step = 2ms) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (pred()) return true;
        std::this_thread::sleep_for(step);
    }
    return pred();
}

bool SendEchoRequest(uint16_t proxy_port, const std::string& msg, std::chrono::milliseconds timeout = 1000ms) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct timeval tv{};
    tv.tv_sec = timeout.count() / 1000;
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    if (!SocketUtils::ParseSockAddr("127.0.0.1", proxy_port, &addr)) {
        ::close(fd);
        return false;
    }

    auto start = std::chrono::steady_clock::now();
    int ret = -1;
    while (std::chrono::steady_clock::now() - start < timeout) {
        ret = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (ret == 0) break;
        if (errno == EINTR) continue;
        if (errno == ECONNREFUSED) {
            std::this_thread::sleep_for(2ms);
            continue;
        }
        break;
    }
    if (ret != 0) {
        ::close(fd);
        return false;
    }

    size_t total_sent = 0;
    while (total_sent < msg.size()) {
        ssize_t n = send(fd, msg.data() + total_sent, msg.size() - total_sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        total_sent += static_cast<size_t>(n);
    }
    if (total_sent != msg.size()) {
        ::close(fd);
        return false;
    }

    std::vector<char> buf(msg.size() + 1, 0);
    size_t total_recvd = 0;
    while (total_recvd < msg.size()) {
        ssize_t n = recv(fd, buf.data() + total_recvd, msg.size() - total_recvd, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (n == 0) break; // EOF
        total_recvd += static_cast<size_t>(n);
    }
    ::close(fd);

    if (total_recvd != msg.size()) return false;
    return std::string(buf.data(), total_recvd) == msg;
}

} // namespace

class HealthCheckerIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        stop_proxy_ = false;
    }

    void TearDown() override {
        StopProxy();
    }

    void StartProxy(L4ProxyServer& proxy) {
        stop_proxy_ = false;
        proxy_thread_ = std::thread([&proxy, this]() {
            while (!stop_proxy_) {
                proxy.RunOnce(std::chrono::milliseconds(1));
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            proxy.Stop();
        });
    }

    void StopProxy() {
        if (proxy_thread_.joinable()) {
            stop_proxy_ = true;
            proxy_thread_.join();
        }
    }

    void RunEngineFor(RingEngine& engine, std::chrono::milliseconds duration) {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < duration) {
            engine.ProcessCompletions();
            engine.Submit();
            std::this_thread::sleep_for(2ms);
        }
    }

    std::atomic<bool> stop_proxy_{false};
    std::thread proxy_thread_;
};

// 1. Healthy backend stays eligible under continuous health checks
TEST_F(HealthCheckerIntegrationTest, HealthyBackendStaysEligible) {
    TestTcpBackend test_server;
    ASSERT_TRUE(test_server.Start());

    RingEngine engine(256);
    auto backend = std::make_shared<Backend>(1, "127.0.0.1", test_server.GetPort());
    backend->SetHealthThresholds(1, 1);
    
    EXPECT_TRUE(backend->IsEligible());

    HealthChecker checker(engine, backend, 30ms, 15ms);
    checker.Start();
    
    RunEngineFor(engine, 100ms);
    
    EXPECT_TRUE(backend->IsEligible());
    EXPECT_EQ(backend->GetHealthState(), HealthState::Healthy);
    EXPECT_EQ(backend->GetConsecutiveFailures(), 0u);
}

// 2. Refused connection transitions to unhealthy across failure threshold
TEST_F(HealthCheckerIntegrationTest, RefusedConnectionTransitionsToUnhealthy) {
    RingEngine engine(256);
    // Port 1 is unassigned on loopback
    auto backend = std::make_shared<Backend>(1, "127.0.0.1", 1);
    backend->SetHealthThresholds(2, 1);
    
    EXPECT_TRUE(backend->IsEligible());

    HealthChecker checker(engine, backend, 40ms, 20ms);
    checker.Start();
    
    // First check fails -> threshold is 2, still eligible
    RunEngineFor(engine, 30ms);
    EXPECT_TRUE(backend->IsEligible());
    EXPECT_EQ(backend->GetConsecutiveFailures(), 1u);
    
    // Second check fails -> crosses failure threshold
    RunEngineFor(engine, 60ms);
    EXPECT_FALSE(backend->IsEligible());
    EXPECT_EQ(backend->GetHealthState(), HealthState::Unhealthy);
}

// 3. Backend starts after Helios: recovers once backend becomes available
TEST_F(HealthCheckerIntegrationTest, BackendStartsAfterHelios) {
    RingEngine engine(256);
    TestTcpBackend test_server;

    uint16_t test_port = 21085;
    test_server.SetPort(test_port);

    auto backend = std::make_shared<Backend>(1, "127.0.0.1", test_port);
    backend->SetHealthThresholds(1, 1);
    
    HealthChecker checker(engine, backend, 30ms, 15ms);
    checker.Start();
    
    // Initial check fails -> transitions to Unhealthy
    RunEngineFor(engine, 50ms);
    EXPECT_FALSE(backend->IsEligible());
    EXPECT_EQ(backend->GetHealthState(), HealthState::Unhealthy);
    
    // Now start the server
    ASSERT_TRUE(test_server.Start());
    
    // Health checker discovers backend and recovers
    RunEngineFor(engine, 70ms);
    EXPECT_TRUE(backend->IsEligible());
    EXPECT_EQ(backend->GetHealthState(), HealthState::Healthy);
}

// 4. Timeout triggers failure when target is unresponsive
TEST_F(HealthCheckerIntegrationTest, TimeoutTriggersFailure) {
    RingEngine engine(256);
    // 240.0.0.1 is reserved (RFC 5735) and silently drops packets, triggering timeout
    auto backend = std::make_shared<Backend>(1, "240.0.0.1", 12345);
    backend->SetHealthThresholds(1, 1);

    EXPECT_TRUE(backend->IsEligible());

    HealthChecker checker(engine, backend, 50ms, 20ms);
    checker.Start();

    // Run engine long enough for the 20ms timeout to fire
    RunEngineFor(engine, 70ms);

    EXPECT_FALSE(backend->IsEligible());
    EXPECT_EQ(backend->GetHealthState(), HealthState::Unhealthy);
    EXPECT_EQ(backend->GetConsecutiveFailures(), 0u);
}

// 5. Backend stops and restarts on the same port
TEST_F(HealthCheckerIntegrationTest, BackendStopsAndRestarts) {
    TestTcpBackend test_server;
    ASSERT_TRUE(test_server.Start());
    uint16_t port = test_server.GetPort();

    RingEngine engine(256);
    auto backend = std::make_shared<Backend>(1, "127.0.0.1", port);
    backend->SetHealthThresholds(1, 1);
    
    HealthChecker checker(engine, backend, 30ms, 15ms);
    checker.Start();
    
    RunEngineFor(engine, 50ms);
    EXPECT_TRUE(backend->IsEligible());
    
    // Stop server -> health check fails
    test_server.Stop();
    RunEngineFor(engine, 50ms);
    EXPECT_FALSE(backend->IsEligible());
    EXPECT_EQ(backend->GetHealthState(), HealthState::Unhealthy);
    
    // Restart server on exact same port
    TestTcpBackend server2;
    server2.SetPort(port);
    ASSERT_TRUE(server2.Start());
    
    RunEngineFor(engine, 60ms);
    EXPECT_TRUE(backend->IsEligible());
    EXPECT_EQ(backend->GetHealthState(), HealthState::Healthy);
}

// 6. Shutdown during outstanding health check cleans up without leaks
TEST_F(HealthCheckerIntegrationTest, ShutdownDuringHealthCheck) {
    RingEngine engine(256);
    auto backend = std::make_shared<Backend>(1, "127.0.0.1", 1);
    backend->SetHealthThresholds(1, 1);
    
    {
        HealthChecker checker(engine, backend, 30ms, 15ms);
        checker.Start();
        engine.ProcessCompletions();
        engine.Submit();
        // checker destroyed here, executing Stop() and cancellation reaping
    }
    
    RunEngineFor(engine, 40ms);
    EXPECT_EQ(engine.InFlightOps(), 0u);
}

// 7. Full 10-step failure injection & recovery lifecycle with live proxy traffic
TEST_F(HealthCheckerIntegrationTest, FullLifecycleFailureInjectionAndRecovery) {
    TestTcpBackend backend1;
    TestTcpBackend backend2;
    ASSERT_TRUE(backend1.Start());
    ASSERT_TRUE(backend2.Start());
    uint16_t port1 = backend1.GetPort();

    L4ProxyServer::Config config;
    config.listen_port = 0; // Ephemeral
    config.lb_algorithm = "round_robin";
    L4ProxyServer proxy(config);

    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", port1);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", backend2.GetPort());
    b1->SetHealthThresholds(2, 2);
    b2->SetHealthThresholds(2, 2);

    proxy.GetLoadBalancer().AddBackend(b1);
    proxy.GetLoadBalancer().AddBackend(b2);

    HealthChecker hc1(proxy.GetEngine(), b1, 25ms, 15ms);
    HealthChecker hc2(proxy.GetEngine(), b2, 25ms, 15ms);
    hc1.Start();
    hc2.Start();

    ASSERT_TRUE(proxy.Start());
    StartProxy(proxy);

    // Step 1: Healthy backend -> traffic flows to both backends
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "Req1"));
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "Req2"));
    EXPECT_GE(backend1.GetEchoCount(), 1u);
    EXPECT_GE(backend2.GetEchoCount(), 1u);

    // Step 2 & 3: Kill backend1 -> health check fails and crosses failure threshold (2) -> becomes Unhealthy
    backend1.Stop();
    bool became_unhealthy = WaitFor([&]() {
        return b1->GetHealthState() == HealthState::Unhealthy;
    }, 1500ms);
    ASSERT_TRUE(became_unhealthy);
    EXPECT_FALSE(b1->IsEligible());

    // Step 4 & 5: New connections stop using unhealthy backend1; other healthy backend2 continues serving
    size_t b1_echoes_before = backend1.GetEchoCount();
    size_t b2_echoes_before = backend2.GetEchoCount();

    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "ReqOnlyB2_1"));
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "ReqOnlyB2_2"));
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "ReqOnlyB2_3"));

    EXPECT_EQ(backend1.GetEchoCount(), b1_echoes_before); // b1 got 0 new echo traffic
    EXPECT_EQ(backend2.GetEchoCount(), b2_echoes_before + 3); // all 3 served by b2

    // Step 6 & 7: Restart failed backend -> recovery checks begin succeeding
    TestTcpBackend backend1_restarted;
    backend1_restarted.SetPort(port1);
    ASSERT_TRUE(backend1_restarted.Start());

    bool entered_recovering = WaitFor([&]() {
        return b1->GetHealthState() == HealthState::Recovering;
    }, 1500ms);
    ASSERT_TRUE(entered_recovering);

    // Step 8: Backend remains ineligible in recovery until recovery threshold (2) is reached
    EXPECT_FALSE(b1->IsEligible());
    // New traffic sent while in recovery still routes to backend2
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "ReqDuringRecovery"));
    EXPECT_EQ(backend2.GetEchoCount(), b2_echoes_before + 4);

    // Step 9: Backend reaches recovery threshold -> becomes Healthy again
    bool became_healthy = WaitFor([&]() {
        return b1->GetHealthState() == HealthState::Healthy;
    }, 1500ms);
    ASSERT_TRUE(became_healthy);
    EXPECT_TRUE(b1->IsEligible());

    // Step 10: New connections can use backend1 again
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "ReqRestored_1"));
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "ReqRestored_2"));
    EXPECT_GE(backend1_restarted.GetEchoCount(), 1u);

    // Cleanup and resource verification
    StopProxy();
    hc1.Stop();
    hc2.Stop();
    RunEngineFor(proxy.GetEngine(), 50ms);
    EXPECT_EQ(proxy.GetActiveConnectionCount(), 0u);
    EXPECT_EQ(proxy.GetBufferPool().CheckedOutCount(), 0u);
    EXPECT_EQ(proxy.GetBufferPool().FreeCount(), proxy.GetBufferPool().TotalBuffers());
}

// 8. One failed check followed by success does not prematurely eject backend
TEST_F(HealthCheckerIntegrationTest, OneFailedCheckFollowedBySuccess) {
    RingEngine engine(256);
    TestTcpBackend server;
    ASSERT_TRUE(server.Start());
    uint16_t port = server.GetPort();

    auto backend = std::make_shared<Backend>(1, "127.0.0.1", port);
    backend->SetHealthThresholds(2, 2);

    HealthChecker checker(engine, backend, 30ms, 15ms);
    checker.Start();

    // Verify initial health
    RunEngineFor(engine, 50ms);
    EXPECT_TRUE(backend->IsEligible());
    EXPECT_EQ(backend->GetConsecutiveFailures(), 0u);

    // Stop server temporarily so exactly 1 failure occurs
    server.Stop();
    bool got_one_fail = WaitFor([&]() {
        engine.ProcessCompletions();
        engine.Submit();
        return backend->GetConsecutiveFailures() == 1u;
    }, 1000ms);
    ASSERT_TRUE(got_one_fail);

    // Still healthy because failure_threshold is 2
    EXPECT_TRUE(backend->IsEligible());
    EXPECT_EQ(backend->GetHealthState(), HealthState::Healthy);

    // Immediately restart server before second failure
    TestTcpBackend server2;
    server2.SetPort(port);
    ASSERT_TRUE(server2.Start());

    // Next check succeeds, resetting consecutive failures to 0
    bool recovered_counter = WaitFor([&]() {
        engine.ProcessCompletions();
        engine.Submit();
        return backend->GetConsecutiveFailures() == 0u;
    }, 1000ms);
    ASSERT_TRUE(recovered_counter);

    EXPECT_TRUE(backend->IsEligible());
    EXPECT_EQ(backend->GetHealthState(), HealthState::Healthy);
}

// 9. Intermittent failures: consecutive counter resets on each success
TEST_F(HealthCheckerIntegrationTest, IntermittentFailuresResistance) {
    RingEngine engine(256);
    auto backend = std::make_shared<Backend>(1, "127.0.0.1", 1);
    backend->SetHealthThresholds(3, 2); // Threshold is 3

    // Sequence: 2 fails, 1 success, 2 fails, 1 success
    backend->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    EXPECT_EQ(backend->GetConsecutiveFailures(), 1u);
    EXPECT_TRUE(backend->IsEligible());

    backend->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    EXPECT_EQ(backend->GetConsecutiveFailures(), 2u);
    EXPECT_TRUE(backend->IsEligible());

    // Intermittent success resets failure counter
    backend->ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(backend->GetConsecutiveFailures(), 0u);
    EXPECT_TRUE(backend->IsEligible());

    backend->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    EXPECT_EQ(backend->GetConsecutiveFailures(), 1u);
    EXPECT_TRUE(backend->IsEligible());

    backend->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    EXPECT_EQ(backend->GetConsecutiveFailures(), 2u);
    EXPECT_TRUE(backend->IsEligible());

    backend->ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(backend->GetConsecutiveFailures(), 0u);
    EXPECT_TRUE(backend->IsEligible());
    EXPECT_EQ(backend->GetHealthState(), HealthState::Healthy);
}

// 10. Repeated failure and recovery cycles
TEST_F(HealthCheckerIntegrationTest, RepeatedFailureRecoveryCycles) {
    TestTcpBackend server;
    ASSERT_TRUE(server.Start());
    uint16_t port = server.GetPort();

    RingEngine engine(256);
    auto backend = std::make_shared<Backend>(1, "127.0.0.1", port);
    backend->SetHealthThresholds(1, 1);

    HealthChecker checker(engine, backend, 20ms, 10ms);
    checker.Start();

    // Execute 3 full failure/recovery cycles
    for (int cycle = 0; cycle < 3; ++cycle) {
        // Run until healthy
        bool is_healthy = WaitFor([&]() {
            engine.ProcessCompletions();
            engine.Submit();
            return backend->GetHealthState() == HealthState::Healthy;
        }, 1000ms);
        ASSERT_TRUE(is_healthy);
        EXPECT_TRUE(backend->IsEligible());

        // Kill backend
        server.Stop();
        bool is_unhealthy = WaitFor([&]() {
            engine.ProcessCompletions();
            engine.Submit();
            return backend->GetHealthState() == HealthState::Unhealthy;
        }, 1000ms);
        ASSERT_TRUE(is_unhealthy);
        EXPECT_FALSE(backend->IsEligible());

        // Restart backend
        server.SetPort(port);
        ASSERT_TRUE(server.Start());
    }

    // Final state check
    bool final_healthy = WaitFor([&]() {
        engine.ProcessCompletions();
        engine.Submit();
        return backend->GetHealthState() == HealthState::Healthy;
    }, 1000ms);
    ASSERT_TRUE(final_healthy);
    EXPECT_TRUE(backend->IsEligible());
}

// 11. Backend flapping: hysteresis prevents premature recovery
TEST_F(HealthCheckerIntegrationTest, BackendFlappingHysteresis) {
    auto backend = std::make_shared<Backend>(1, "127.0.0.1", 1);
    backend->SetHealthThresholds(2, 2);

    // Cross failure threshold -> Unhealthy
    backend->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    backend->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    ASSERT_EQ(backend->GetHealthState(), HealthState::Unhealthy);
    EXPECT_FALSE(backend->IsEligible());

    // 1st recovery success -> enters Recovering, still ineligible
    backend->ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(backend->GetHealthState(), HealthState::Recovering);
    EXPECT_FALSE(backend->IsEligible());
    EXPECT_EQ(backend->GetConsecutiveSuccesses(), 1u);

    // Flap: failure occurs before second success
    backend->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    // Immediately drops back to Unhealthy, consecutive successes reset
    EXPECT_EQ(backend->GetHealthState(), HealthState::Unhealthy);
    EXPECT_FALSE(backend->IsEligible());
    EXPECT_EQ(backend->GetConsecutiveSuccesses(), 0u);

    // Another success -> enters Recovering again
    backend->ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(backend->GetHealthState(), HealthState::Recovering);
    EXPECT_FALSE(backend->IsEligible());

    // Second consecutive success -> now reaches threshold and becomes Healthy
    backend->ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(backend->GetHealthState(), HealthState::Healthy);
    EXPECT_TRUE(backend->IsEligible());
}

// 12. Backend unavailable at Helios startup
TEST_F(HealthCheckerIntegrationTest, BackendUnavailableAtHeliosStartup) {
    TestTcpBackend healthy_server;
    ASSERT_TRUE(healthy_server.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.lb_algorithm = "round_robin";
    L4ProxyServer proxy(config);

    // Backend 1 is on port 1 (unassigned/unavailable at startup)
    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", 1);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", healthy_server.GetPort());
    b1->SetHealthThresholds(1, 1);
    b2->SetHealthThresholds(1, 1);

    proxy.GetLoadBalancer().AddBackend(b1);
    proxy.GetLoadBalancer().AddBackend(b2);

    HealthChecker hc1(proxy.GetEngine(), b1, 25ms, 15ms);
    HealthChecker hc2(proxy.GetEngine(), b2, 25ms, 15ms);
    hc1.Start();
    hc2.Start();

    ASSERT_TRUE(proxy.Start());
    StartProxy(proxy);

    // Wait for initial check to mark b1 Unhealthy
    bool b1_unhealthy = WaitFor([&]() {
        return b1->GetHealthState() == HealthState::Unhealthy;
    }, 1500ms);
    ASSERT_TRUE(b1_unhealthy);
    EXPECT_FALSE(b1->IsEligible());

    // Healthy backend routes all traffic with 100% success
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "StartupTest_1"));
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "StartupTest_2"));
    EXPECT_GE(healthy_server.GetEchoCount(), 2u);

    StopProxy();
    hc1.Stop();
    hc2.Stop();
    RunEngineFor(proxy.GetEngine(), 50ms);
    EXPECT_EQ(proxy.GetActiveConnectionCount(), 0u);
}

// 13. Backend becoming available later after startup failure
TEST_F(HealthCheckerIntegrationTest, BackendBecomingAvailableLater) {
    TestTcpBackend late_server;
    uint16_t late_port = 21086;
    late_server.SetPort(late_port);

    L4ProxyServer::Config config;
    config.listen_port = 0;
    L4ProxyServer proxy(config);

    auto backend = std::make_shared<Backend>(1, "127.0.0.1", late_port);
    backend->SetHealthThresholds(1, 1);
    proxy.GetLoadBalancer().AddBackend(backend);

    HealthChecker hc(proxy.GetEngine(), backend, 25ms, 15ms);
    hc.Start();

    ASSERT_TRUE(proxy.Start());
    StartProxy(proxy);

    // Initial check fails
    bool became_unhealthy = WaitFor([&]() {
        return backend->GetHealthState() == HealthState::Unhealthy;
    }, 1500ms);
    ASSERT_TRUE(became_unhealthy);

    // Now start the late server
    ASSERT_TRUE(late_server.Start());

    // Health checker discovers it and marks it Healthy
    bool became_healthy = WaitFor([&]() {
        return backend->GetHealthState() == HealthState::Healthy;
    }, 1500ms);
    ASSERT_TRUE(became_healthy);
    EXPECT_TRUE(backend->IsEligible());

    // Traffic flows to the newly available backend
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "LateAvailableEcho"));
    EXPECT_GE(late_server.GetEchoCount(), 1u);

    StopProxy();
    hc.Stop();
    RunEngineFor(proxy.GetEngine(), 50ms);
    EXPECT_EQ(proxy.GetActiveConnectionCount(), 0u);
}

// 14. Backend disappearing during active client traffic
TEST_F(HealthCheckerIntegrationTest, BackendDisappearingDuringActiveClientTraffic) {
    TestTcpBackend backend1;
    TestTcpBackend backend2;
    ASSERT_TRUE(backend1.Start());
    ASSERT_TRUE(backend2.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.lb_algorithm = "round_robin";
    L4ProxyServer proxy(config);

    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", backend1.GetPort());
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", backend2.GetPort());
    b1->SetHealthThresholds(1, 1);
    b2->SetHealthThresholds(1, 1);

    proxy.GetLoadBalancer().AddBackend(b1);
    proxy.GetLoadBalancer().AddBackend(b2);

    HealthChecker hc1(proxy.GetEngine(), b1, 20ms, 10ms);
    HealthChecker hc2(proxy.GetEngine(), b2, 20ms, 10ms);
    hc1.Start();
    hc2.Start();

    ASSERT_TRUE(proxy.Start());
    StartProxy(proxy);

    // Connect client to backend 1
    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(client_fd, 0);
    sockaddr_in addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &addr));
    ASSERT_EQ(connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    // Exchange ping
    std::string ping = "ActiveTrafficPing";
    ssize_t s_ret = send(client_fd, ping.data(), ping.size(), 0);
    ASSERT_EQ(s_ret, static_cast<ssize_t>(ping.size()));
    char buf[64]{};
    ssize_t r_ret = recv(client_fd, buf, sizeof(buf), 0);
    ASSERT_EQ(r_ret, static_cast<ssize_t>(ping.size()));
    EXPECT_EQ(std::string(buf, static_cast<size_t>(r_ret)), ping);
    EXPECT_GE(b1->GetActiveConnections(), 1u);

    // Abruptly terminate backend 1
    backend1.Stop();

    // Health checker detects disappearance
    bool b1_unhealthy = WaitFor([&]() {
        return b1->GetHealthState() == HealthState::Unhealthy;
    }, 1500ms);
    ASSERT_TRUE(b1_unhealthy);

    // Close client socket
    ::close(client_fd);

    // Bounded wait for active connections on b1 to reach 0
    bool conns_drained = WaitFor([&]() {
        return b1->GetActiveConnections() == 0u;
    }, 1500ms);
    EXPECT_TRUE(conns_drained);

    // New connection automatically routes to surviving backend 2
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "ReqToSurvivingB2"));
    EXPECT_GE(backend2.GetEchoCount(), 1u);

    StopProxy();
    hc1.Stop();
    hc2.Stop();
    RunEngineFor(proxy.GetEngine(), 50ms);
    EXPECT_EQ(proxy.GetActiveConnectionCount(), 0u);
}

// 15. Helios shutdown during outstanding health check cleans all resources
TEST_F(HealthCheckerIntegrationTest, ShutdownDuringOutstandingProxyHealthCheck) {
    L4ProxyServer::Config config;
    config.listen_port = 0;
    L4ProxyServer proxy(config);

    // Target dropping blackhole address
    auto b1 = std::make_shared<Backend>(1, "240.0.0.1", 12345);
    b1->SetHealthThresholds(1, 1);
    proxy.GetLoadBalancer().AddBackend(b1);

    HealthChecker hc(proxy.GetEngine(), b1, 500ms, 200ms);
    hc.Start();

    ASSERT_TRUE(proxy.Start());
    StartProxy(proxy);

    // Wait a brief instant for connect/timeout to be actively in flight
    std::this_thread::sleep_for(10ms);

    // Abruptly shut down while check is outstanding
    StopProxy();
    hc.Stop();
    RunEngineFor(proxy.GetEngine(), 50ms);

    // Verify clean shutdown with zero memory or connection leaks
    EXPECT_FALSE(proxy.IsRunning());
    EXPECT_EQ(proxy.GetActiveConnectionCount(), 0u);
    EXPECT_LE(proxy.GetEngine().InFlightOps(), 1u); // At most proxy listener accept SQE; health checker left 0
    EXPECT_EQ(proxy.GetBufferPool().CheckedOutCount(), 0u);
    EXPECT_EQ(proxy.GetBufferPool().FreeCount(), proxy.GetBufferPool().TotalBuffers());
}

// 16. HTTP health check probing and automatic failover
TEST_F(HealthCheckerIntegrationTest, HttpHealthCheckProbingAndAutomaticFailover) {
    class HttpProbeBackend {
    public:
        explicit HttpProbeBackend(int code = 200) : code_(code) {}
        ~HttpProbeBackend() { Stop(); }

        bool Start() {
            if (running_.load()) return true;
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) return false;
            int opt = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");
            addr.sin_port = 0;
            if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                close(fd);
                return false;
            }
            socklen_t len = sizeof(addr);
            getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
            port_ = ntohs(addr.sin_port);

            struct timeval tv{0, 20000};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            listen(fd, 64);
            listen_fd_.store(fd);
            running_.store(true);
            thread_ = std::thread([this]() {
                while (running_.load()) {
                    int lfd = listen_fd_.load();
                    if (lfd < 0) break;
                    sockaddr_in caddr{};
                    socklen_t clen = sizeof(caddr);
                    int cfd = accept(lfd, reinterpret_cast<sockaddr*>(&caddr), &clen);
                    if (cfd < 0) continue;
                    char buf[512];
                    recv(cfd, buf, sizeof(buf) - 1, 0);
                    int c = code_.load();
                    std::string resp = (c == 200) ? "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK"
                                                  : "HTTP/1.1 500 Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                    send(cfd, resp.data(), resp.size(), 0);
                    close(cfd);
                }
            });
            return true;
        }

        void Stop() {
            if (running_.exchange(false)) {
                int fd = listen_fd_.exchange(-1);
                if (fd >= 0) {
                    shutdown(fd, SHUT_RDWR);
                    close(fd);
                }
                if (thread_.joinable()) thread_.join();
            }
        }

        void SetCode(int code) { code_.store(code); }
        [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }

    private:
        std::atomic<int> listen_fd_{-1};
        uint16_t port_{0};
        std::atomic<bool> running_{false};
        std::atomic<int> code_{200};
        std::thread thread_;
    };

    HttpProbeBackend b1(200);
    ASSERT_TRUE(b1.Start());

    RingEngine engine(64);
    auto backend1 = std::make_shared<Backend>(1, "127.0.0.1", b1.GetPort(), 1);
    backend1->SetHealthThresholds(2, 1);

    HealthChecker hc1(engine, backend1, 20ms, 40ms, HealthCheckType::Http, "/healthz", 200);
    hc1.Start();

    // Verify b1 initially probes as healthy
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 1000ms) {
        engine.ProcessCompletions();
        engine.Submit();
        if (backend1->GetHealthState() == HealthState::Healthy) break;
        std::this_thread::sleep_for(2ms);
    }
    EXPECT_EQ(backend1->GetHealthState(), HealthState::Healthy);

    // Now make b1 return 500 -> probe should fail and transition backend to Unhealthy
    b1.SetCode(500);
    start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 2000ms) {
        engine.ProcessCompletions();
        engine.Submit();
        if (backend1->GetHealthState() == HealthState::Unhealthy) break;
        std::this_thread::sleep_for(2ms);
    }
    EXPECT_EQ(backend1->GetHealthState(), HealthState::Unhealthy);
    EXPECT_FALSE(backend1->IsEligible());

    hc1.Stop();
    RunEngineFor(engine, 50ms);
}

// 17. Immediate BackendConnectionPool purging on health ejection
TEST_F(HealthCheckerIntegrationTest, ImmediateConnectionPoolPurgeOnHealthEjection) {
    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_connection_pooling = true;
    config.connection_pool_config.max_idle_per_backend = 10;
    config.connection_pool_config.max_total_idle = 20;
    L4ProxyServer proxy(config);

    auto b1 = std::make_shared<Backend>(50, "127.0.0.1", 9999, 1);
    proxy.AddBackend(b1);

    // Simulate idle connections in the pool for b1
    int fake_sock1 = SocketUtils::CreateTcpSocket();
    int fake_sock2 = SocketUtils::CreateTcpSocket();
    ASSERT_GE(fake_sock1, 0);
    ASSERT_GE(fake_sock2, 0);

    ASSERT_TRUE(proxy.GetBackendPool()->Release(50, fake_sock1));
    ASSERT_TRUE(proxy.GetBackendPool()->Release(50, fake_sock2));
    EXPECT_EQ(proxy.GetBackendPool()->IdleCountForBackend(50), 2u);

    // Trigger PurgeBackend (invoked upon health ejection)
    proxy.PurgeBackend(50);

    // Idle connections for backend 50 must immediately be 0
    EXPECT_EQ(proxy.GetBackendPool()->IdleCountForBackend(50), 0u);
    EXPECT_EQ(proxy.GetBackendPool()->TotalIdleCount(), 0u);
}
