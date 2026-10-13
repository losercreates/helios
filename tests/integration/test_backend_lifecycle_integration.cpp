#include <gtest/gtest.h>
#include "lb/circuit_breaker.hpp"
#include "lb/round_robin.hpp"
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

using namespace helios;
using namespace std::chrono_literals;

namespace {

template <typename Predicate>
bool WaitFor(Predicate&& pred, std::chrono::milliseconds timeout = 2500ms, std::chrono::milliseconds step = 2ms) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (pred()) return true;
        std::this_thread::sleep_for(step);
    }
    return pred();
}

bool SendEchoRequest(uint16_t proxy_port, const std::string& msg, std::string* out_resp = nullptr, std::chrono::milliseconds timeout = 1000ms) {
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

    std::string resp;
    resp.resize(msg.size());
    size_t total_recv = 0;
    while (total_recv < msg.size()) {
        ssize_t n = recv(fd, resp.data() + total_recv, msg.size() - total_recv, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        total_recv += static_cast<size_t>(n);
    }
    ::close(fd);

    if (total_recv == msg.size() && resp == msg) {
        if (out_resp) *out_resp = resp;
        return true;
    }
    return false;
}

} // namespace

class BackendLifecycleIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        stop_proxy_ = false;
        pause_proxy_ = false;
        proxy_paused_ = false;
    }

    void TearDown() override {
        StopProxyLoop();
    }

    void StartProxyLoop(L4ProxyServer& proxy) {
        stop_proxy_ = false;
        pause_proxy_ = false;
        proxy_paused_ = false;
        proxy_thread_ = std::thread([&proxy, this]() {
            while (!stop_proxy_) {
                if (pause_proxy_) {
                    proxy_paused_ = true;
                    while (pause_proxy_ && !stop_proxy_) {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                    proxy_paused_ = false;
                }
                if (!stop_proxy_) {
                    proxy.RunOnce(std::chrono::milliseconds(1));
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
            proxy.Stop();
        });
    }

    void PauseProxyLoop() {
        if (proxy_thread_.joinable()) {
            pause_proxy_ = true;
            while (!proxy_paused_ && !stop_proxy_) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }

    void ResumeProxyLoop() {
        pause_proxy_ = false;
    }

    void StopProxyLoop() {
        if (proxy_thread_.joinable()) {
            stop_proxy_ = true;
            proxy_thread_.join();
        }
    }

    std::atomic<bool> stop_proxy_{false};
    std::atomic<bool> pause_proxy_{false};
    std::atomic<bool> proxy_paused_{false};
    std::thread proxy_thread_;
};

// 1. Traffic redistribution on backend failure and subsequent recovery
TEST_F(BackendLifecycleIntegrationTest, TrafficRedistributionOnBackendFailureAndRecovery) {
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
    b1->SetHealthThresholds(1, 2); // 1 failure -> unhealthy, 2 successes -> healthy
    b2->SetHealthThresholds(1, 2);

    proxy.GetLoadBalancer().AddBackend(b1);
    proxy.GetLoadBalancer().AddBackend(b2);

    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    // Initial baseline: both healthy, traffic distributed
    ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "msg-1"));
    ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "msg-2"));
    EXPECT_GT(backend1.GetEchoCount() + backend2.GetEchoCount(), 0u);

    // Fail backend 1
    backend1.Stop();
    b1->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    EXPECT_EQ(b1->GetHealthState(), HealthState::Unhealthy);
    EXPECT_FALSE(b1->CanAcceptNewTraffic());

    size_t b2_start_echoes = backend2.GetEchoCount();
    // Send 6 requests -> all must reach backend 2
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "msg-failover-" + std::to_string(i)));
    }
    EXPECT_EQ(backend2.GetEchoCount(), b2_start_echoes + 6);

    uint16_t b1_port = backend1.GetPort();
    // Restart backend 1 on same port using fresh instance
    TestTcpBackend backend1_restarted;
    backend1_restarted.SetPort(b1_port);
    ASSERT_TRUE(backend1_restarted.Start());
    // 1st health success -> Recovering (still ineligible for regular traffic)
    b1->ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(b1->GetHealthState(), HealthState::Recovering);
    EXPECT_FALSE(b1->CanAcceptNewTraffic());

    // 2nd health success -> Healthy!
    b1->ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(b1->GetHealthState(), HealthState::Healthy);
    EXPECT_TRUE(b1->CanAcceptNewTraffic());

    backend1_restarted.ResetEchoCount();
    backend2.ResetEchoCount();

    // Now traffic distributes across both again
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "msg-recovered-" + std::to_string(i)));
    }
    EXPECT_GT(backend1_restarted.GetEchoCount(), 0u);
    EXPECT_GT(backend2.GetEchoCount(), 0u);
    EXPECT_EQ(backend1_restarted.GetEchoCount() + backend2.GetEchoCount(), 6u);
    StopProxyLoop();
}

// 2. Traffic redistribution on circuit trip and recovery
TEST_F(BackendLifecycleIntegrationTest, TrafficRedistributionOnCircuitTripAndRecovery) {
    TestTcpBackend backend1;
    TestTcpBackend backend2;
    ASSERT_TRUE(backend1.Start());
    ASSERT_TRUE(backend2.Start());

    uint16_t b1_port = backend1.GetPort();

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.lb_algorithm = "round_robin";
    L4ProxyServer proxy(config);

    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", b1_port);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", backend2.GetPort());

    CircuitBreakerConfig cb_cfg;
    cb_cfg.failure_threshold = 1;
    cb_cfg.cooldown = 40ms;
    cb_cfg.half_open_max_probes = 1;
    b1->ConfigureCircuitBreaker(cb_cfg);

    proxy.GetLoadBalancer().AddBackend(b1);
    proxy.GetLoadBalancer().AddBackend(b2);

    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    // Baseline request to b1
    ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "test-initial"));

    // Stop backend1 -> next connection routed to b1 will fail and trip its circuit
    backend1.Stop();

    // Trigger connect failure on b1
    for (int i = 0; i < 4; ++i) {
        SendEchoRequest(proxy.GetPort(), "trigger-cb-trip", nullptr, 50ms);
    }

    ASSERT_TRUE(WaitFor([&]() {
        return b1->GetCircuitState() == CircuitState::Open;
    }));
    EXPECT_FALSE(b1->CanAcceptNewTraffic());

    // While b1 circuit is OPEN, send 5 requests -> all route to b2 without stalls
    backend2.ResetEchoCount();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "rerouted-" + std::to_string(i)));
    }
    EXPECT_EQ(backend2.GetEchoCount(), 5u);

    // Restart backend1 on identical port using fresh instance
    TestTcpBackend backend1_restarted;
    backend1_restarted.SetPort(b1_port);
    ASSERT_TRUE(backend1_restarted.Start());

    // Wait for cooldown to expire -> transitions to HalfOpen
    ASSERT_TRUE(WaitFor([&]() {
        return b1->GetCircuitState() == CircuitState::HalfOpen;
    }));
    EXPECT_TRUE(b1->CanAcceptNewTraffic());

    // Probe request succeeds -> closes circuit
    backend1_restarted.ResetEchoCount();
    backend2.ResetEchoCount();
    ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "probe-msg"));
    
    ASSERT_TRUE(WaitFor([&]() {
        return b1->GetCircuitState() == CircuitState::Closed;
    }));

    // Traffic continues flowing across both backends
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "steady-" + std::to_string(i)));
    }
    EXPECT_GT(backend1_restarted.GetEchoCount(), 0u);
    EXPECT_GT(backend2.GetEchoCount(), 0u);
    StopProxyLoop();
}

// 3. Backend draining preserves existing connections while rerouting new traffic
TEST_F(BackendLifecycleIntegrationTest, BackendDrainingPreservesExistingConnections) {
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
    proxy.GetLoadBalancer().AddBackend(b1);
    proxy.GetLoadBalancer().AddBackend(b2);

    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &target_addr));

    // Connect Client 1 -> lands on b1
    int fd1 = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(fd1, 0);
    ASSERT_EQ(connect(fd1, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    ASSERT_TRUE(WaitFor([&]() { return b1->GetActiveConnections() == 1u; }));
    EXPECT_EQ(b2->GetActiveConnections(), 0u);

    // Now administratively mark b1 as draining
    b1->SetDraining(true);
    EXPECT_TRUE(b1->IsDraining());
    EXPECT_FALSE(b1->CanAcceptNewTraffic());

    // CRITICAL: Prove Client 1's existing connection to b1 is NOT force-closed!
    std::string msg = "live-data-on-draining-backend";
    ASSERT_EQ(send(fd1, msg.data(), msg.size(), 0), static_cast<ssize_t>(msg.size()));
    std::array<char, 64> buf{};
    ssize_t n = recv(fd1, buf.data(), buf.size(), 0);
    ASSERT_EQ(n, static_cast<ssize_t>(msg.size()));
    EXPECT_EQ(std::string(buf.data(), static_cast<size_t>(n)), msg);
    EXPECT_EQ(b1->GetActiveConnections(), 1u);

    // Send 4 new client connections -> 100% must route to b2, zero to b1
    backend2.ResetEchoCount();
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "new-client-" + std::to_string(i)));
    }
    EXPECT_EQ(backend2.GetEchoCount(), 4u);
    EXPECT_EQ(b1->GetActiveConnections(), 1u); // b1 active connections unchanged

    // Now Client 1 finishes and cleanly closes
    ::close(fd1);
    ASSERT_TRUE(WaitFor([&]() { return b1->GetActiveConnections() == 0u; }));

    // Zero active connections remaining on draining backend
    EXPECT_EQ(b1->GetActiveConnections(), 0u);
    StopProxyLoop();
}

// 4. Traffic redistribution on dynamic backend removal
TEST_F(BackendLifecycleIntegrationTest, TrafficRedistributionOnBackendRemoval) {
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
    proxy.GetLoadBalancer().AddBackend(b1);
    proxy.GetLoadBalancer().AddBackend(b2);

    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    // Baseline request
    ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "msg-0"));

    // Dynamically remove backend 1 from load balancer safely
    PauseProxyLoop();
    EXPECT_TRUE(proxy.GetLoadBalancer().RemoveBackend(1));
    EXPECT_EQ(proxy.GetLoadBalancer().GetBackendCount(), 1u);
    ResumeProxyLoop();

    // Send 5 requests -> all route to backend 2
    backend1.ResetEchoCount();
    backend2.ResetEchoCount();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "post-removal-" + std::to_string(i)));
    }
    EXPECT_EQ(backend1.GetEchoCount(), 0u);
    EXPECT_EQ(backend2.GetEchoCount(), 5u);
    StopProxyLoop();
}

// 5. All backends unavailable rejection and recovery
TEST_F(BackendLifecycleIntegrationTest, AllBackendsUnavailableRejectionAndRecovery) {
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
    proxy.GetLoadBalancer().AddBackend(b1);
    proxy.GetLoadBalancer().AddBackend(b2);

    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    // Make both unavailable (draining)
    b1->SetDraining(true);
    b2->SetDraining(true);
    EXPECT_FALSE(b1->CanAcceptNewTraffic());
    EXPECT_FALSE(b2->CanAcceptNewTraffic());

    // Connect client -> should be closed cleanly without hanging proxy
    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &target_addr));
    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(client_fd, 0);
    struct timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000; // 200ms
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int ret = connect(client_fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr));
    if (ret == 0) {
        // Connected to listener, but proxy immediately closes it because 0 backends available
        char buf[8];
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        EXPECT_LE(n, 0); // EOF or error
    }
    ::close(client_fd);

    // Re-enable backend 2
    b2->SetDraining(false);
    EXPECT_TRUE(b2->CanAcceptNewTraffic());

    // Now new connection immediately succeeds
    ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "recovered-traffic"));
    EXPECT_GT(backend2.GetEchoCount(), 0u);
    StopProxyLoop();
}

// 6. Comprehensive backend full lifecycle orchestration with 3 real backends
TEST_F(BackendLifecycleIntegrationTest, ComprehensiveBackendLifecycle) {
    TestTcpBackend b1_srv;
    TestTcpBackend b2_srv;
    TestTcpBackend b3_srv;
    ASSERT_TRUE(b1_srv.Start());
    ASSERT_TRUE(b2_srv.Start());
    ASSERT_TRUE(b3_srv.Start());

    uint16_t b2_port = b2_srv.GetPort();

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.lb_algorithm = "round_robin";
    L4ProxyServer proxy(config);

    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", b1_srv.GetPort());
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", b2_port);
    auto b3 = std::make_shared<Backend>(3, "127.0.0.1", b3_srv.GetPort());

    CircuitBreakerConfig cb2_cfg;
    cb2_cfg.failure_threshold = 1;
    cb2_cfg.cooldown = 40ms;
    cb2_cfg.half_open_max_probes = 1;
    b2->ConfigureCircuitBreaker(cb2_cfg);

    proxy.GetLoadBalancer().AddBackend(b1);
    proxy.GetLoadBalancer().AddBackend(b2);
    proxy.GetLoadBalancer().AddBackend(b3);

    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    // Step 1: All 3 active, traffic distributed
    for (int i = 0; i < 9; ++i) {
        ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "msg-step1-" + std::to_string(i)));
    }
    EXPECT_GT(b1_srv.GetEchoCount(), 0u);
    EXPECT_GT(b2_srv.GetEchoCount(), 0u);
    EXPECT_GT(b3_srv.GetEchoCount(), 0u);

    // Step 2: Establish active long-lived connection on B1, then drain B1
    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &target_addr));
    int b1_client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(b1_client_fd, 0);
    struct timeval b1_tv{};
    b1_tv.tv_sec = 1;
    setsockopt(b1_client_fd, SOL_SOCKET, SO_RCVTIMEO, &b1_tv, sizeof(b1_tv));
    ASSERT_EQ(connect(b1_client_fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);
    ASSERT_TRUE(WaitFor([&]() { return b1->GetActiveConnections() >= 1u; }));
    b1->SetDraining(true);

    // Active client continues communicating
    std::string test_payload = "b1-active-session";
    send(b1_client_fd, test_payload.data(), test_payload.size(), 0);
    std::array<char, 32> resp_buf{};
    ssize_t n = recv(b1_client_fd, resp_buf.data(), resp_buf.size(), 0);
    EXPECT_GT(n, 0);

    // Step 3: Stop B2 -> circuit trips to OPEN
    b2_srv.Stop();
    for (int i = 0; i < 4; ++i) {
        SendEchoRequest(proxy.GetPort(), "trip-b2", nullptr, 50ms);
    }
    ASSERT_TRUE(WaitFor([&]() { return b2->GetCircuitState() == CircuitState::Open; }));

    // Step 4: With B1 draining and B2 circuit open, ALL traffic must route to B3
    b3_srv.ResetEchoCount();
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "solo-b3-" + std::to_string(i)));
    }
    EXPECT_EQ(b3_srv.GetEchoCount(), 6u);

    // Step 5: Restart B2 -> recovers through half-open probe
    TestTcpBackend b2_restarted;
    b2_restarted.SetPort(b2_port);
    ASSERT_TRUE(b2_restarted.Start());
    ASSERT_TRUE(WaitFor([&]() { return b2->GetCircuitState() == CircuitState::HalfOpen; }));

    // Probe succeeds, circuit closes
    ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "b2-probe"));
    ASSERT_TRUE(WaitFor([&]() { return b2->GetCircuitState() == CircuitState::Closed; }));

    // Step 6: Close client on B1 -> B1 active connections drops to 0, then remove B1
    ::close(b1_client_fd);
    ASSERT_TRUE(WaitFor([&]() { return b1->GetActiveConnections() == 0u; }));
    PauseProxyLoop();
    EXPECT_TRUE(proxy.GetLoadBalancer().RemoveBackend(1));
    ResumeProxyLoop();

    // Traffic continues flowing smoothly between B2 and B3
    b2_restarted.ResetEchoCount();
    b3_srv.ResetEchoCount();
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(SendEchoRequest(proxy.GetPort(), "b2-b3-steady-" + std::to_string(i)));
    }
    EXPECT_GT(b2_restarted.GetEchoCount(), 0u);
    EXPECT_GT(b3_srv.GetEchoCount(), 0u);
    EXPECT_EQ(b2_restarted.GetEchoCount() + b3_srv.GetEchoCount(), 6u);
    StopProxyLoop();
}
