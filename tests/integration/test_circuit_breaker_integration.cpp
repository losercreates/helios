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

class CircuitBreakerIntegrationTest : public ::testing::Test {
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

    std::atomic<bool> stop_proxy_{false};
    std::thread proxy_thread_;
};

// 1. Full 10-step Circuit Breaker failure, trip, cooldown, and recovery lifecycle
TEST_F(CircuitBreakerIntegrationTest, FullCircuitBreakerLifecycle) {
    TestTcpBackend backend1;
    TestTcpBackend backend2;
    ASSERT_TRUE(backend1.Start());
    ASSERT_TRUE(backend2.Start());
    uint16_t port1 = backend1.GetPort();

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.lb_algorithm = "round_robin";
    L4ProxyServer proxy(config);

    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", port1);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", backend2.GetPort());

    CircuitBreakerConfig cb_config;
    cb_config.failure_threshold = 2;
    cb_config.cooldown = 100ms;
    cb_config.half_open_max_probes = 1;
    cb_config.success_threshold = 1;
    b1->ConfigureCircuitBreaker(cb_config);
    b2->ConfigureCircuitBreaker(cb_config);

    proxy.GetLoadBalancer().AddBackend(b1);
    proxy.GetLoadBalancer().AddBackend(b2);

    ASSERT_TRUE(proxy.Start());
    StartProxy(proxy);

    // Step 1: Healthy backends -> traffic flows to both
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "Req1"));
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "Req2"));
    EXPECT_GE(backend1.GetEchoCount() + backend2.GetEchoCount(), 2u);

    // Step 2: Kill backend 1 -> backend begins failing
    backend1.Stop();

    // Trigger connection failures on backend 1
    // In round-robin, requests alternate between b1 and b2
    for (int i = 0; i < 4; ++i) {
        SendEchoRequest(proxy.GetPort(), "ProbeReq", 50ms);
    }

    // Step 3: Circuit opens on backend 1
    bool circuit_opened = WaitFor([&]() {
        return b1->GetCircuitState() == CircuitState::Open;
    }, 1500ms);
    ASSERT_TRUE(circuit_opened);

    // Step 4 & 5: New traffic avoids backend 1; healthy backend 2 receives all traffic
    backend2.ResetEchoCount();
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "TrafficToB2_" + std::to_string(i)));
    }
    EXPECT_EQ(backend2.GetEchoCount(), 4u);
    EXPECT_EQ(b1->GetCircuitState(), CircuitState::Open);

    // Step 6: Wait for cooldown interval (100ms) to expire -> enters HalfOpen
    bool entered_half_open = WaitFor([&]() {
        return b1->GetCircuitState() == CircuitState::HalfOpen;
    }, 1500ms);
    ASSERT_TRUE(entered_half_open);

    // Step 7 & 8: Restart backend 1 on exact same port
    TestTcpBackend backend1_restored;
    backend1_restored.SetPort(port1);
    ASSERT_TRUE(backend1_restored.Start());

    // Step 9: Send request through proxy; serves as probe connection to backend 1
    // Probing succeeds and circuit transitions to Closed
    bool circuit_closed = WaitFor([&]() {
        SendEchoRequest(proxy.GetPort(), "ProbeRecoveryEcho");
        return b1->GetCircuitState() == CircuitState::Closed;
    }, 1500ms);
    EXPECT_TRUE(circuit_closed);

    // Step 10: Backend 1 is now Closed and receives normal traffic again
    backend1_restored.ResetEchoCount();
    backend2.ResetEchoCount();
    for (int i = 0; i < 6; ++i) {
        EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "NormalTraffic_" + std::to_string(i)));
    }
    EXPECT_GE(backend1_restored.GetEchoCount(), 1u);
    EXPECT_GE(backend2.GetEchoCount(), 1u);

    StopProxy();
    EXPECT_EQ(proxy.GetActiveConnectionCount(), 0u);
}

// 2. Failed probe in HalfOpen trips circuit back to Open
TEST_F(CircuitBreakerIntegrationTest, FailedProbeReopensCircuit) {
    TestTcpBackend backend1;
    TestTcpBackend backend2;
    ASSERT_TRUE(backend1.Start());
    ASSERT_TRUE(backend2.Start());
    uint16_t port1 = backend1.GetPort();

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.lb_algorithm = "round_robin";
    L4ProxyServer proxy(config);

    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", port1);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", backend2.GetPort());

    CircuitBreakerConfig cb_config;
    cb_config.failure_threshold = 1;
    cb_config.cooldown = 100ms;
    b1->ConfigureCircuitBreaker(cb_config);
    b2->ConfigureCircuitBreaker(cb_config);

    proxy.GetLoadBalancer().AddBackend(b1);
    proxy.GetLoadBalancer().AddBackend(b2);

    ASSERT_TRUE(proxy.Start());
    StartProxy(proxy);

    // Stop backend 1
    backend1.Stop();

    // Trip b1
    WaitFor([&]() {
        SendEchoRequest(proxy.GetPort(), "TripB1", 50ms);
        return b1->GetCircuitState() == CircuitState::Open;
    }, 1500ms);
    ASSERT_EQ(b1->GetCircuitState(), CircuitState::Open);

    // Wait for cooldown to expire
    bool entered_half_open = WaitFor([&]() {
        return b1->GetCircuitState() == CircuitState::HalfOpen;
    }, 1500ms);
    ASSERT_TRUE(entered_half_open);

    // Do NOT restart backend 1. Send probe. Connect fails.
    // Circuit must trip back to Open!
    bool returned_to_open = WaitFor([&]() {
        SendEchoRequest(proxy.GetPort(), "FailedProbe", 50ms);
        return b1->GetCircuitState() == CircuitState::Open;
    }, 1500ms);
    EXPECT_TRUE(returned_to_open);

    // Backend 2 continues serving all traffic seamlessly
    backend2.ResetEchoCount();
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "B2Continues"));
    }
    EXPECT_EQ(backend2.GetEchoCount(), 4u);

    StopProxy();
    EXPECT_EQ(proxy.GetActiveConnectionCount(), 0u);
}

// 3. Repeated backend flapping across multiple failure-recovery cycles
TEST_F(CircuitBreakerIntegrationTest, RepeatedBackendFlapping) {
    TestTcpBackend backend1;
    TestTcpBackend backend2;
    ASSERT_TRUE(backend1.Start());
    ASSERT_TRUE(backend2.Start());
    uint16_t port1 = backend1.GetPort();

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.lb_algorithm = "round_robin";
    L4ProxyServer proxy(config);

    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", port1);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", backend2.GetPort());

    CircuitBreakerConfig cb_config;
    cb_config.failure_threshold = 1;
    cb_config.cooldown = 50ms;
    b1->ConfigureCircuitBreaker(cb_config);

    proxy.GetLoadBalancer().AddBackend(b1);
    proxy.GetLoadBalancer().AddBackend(b2);

    ASSERT_TRUE(proxy.Start());
    StartProxy(proxy);

    for (int cycle = 0; cycle < 3; ++cycle) {
        // Kill backend 1
        backend1.Stop();

        // Trip b1 to Open
        bool opened = WaitFor([&]() {
            SendEchoRequest(proxy.GetPort(), "FlapTrip", 30ms);
            return b1->GetCircuitState() == CircuitState::Open;
        }, 1500ms);
        ASSERT_TRUE(opened);

        // Cooldown expires -> HalfOpen
        bool half_open = WaitFor([&]() {
            return b1->GetCircuitState() == CircuitState::HalfOpen;
        }, 1500ms);
        ASSERT_TRUE(half_open);

        // Restart backend 1
        TestTcpBackend restarter;
        restarter.SetPort(port1);
        ASSERT_TRUE(restarter.Start());

        // Probe succeeds -> Closed
        bool closed = WaitFor([&]() {
            SendEchoRequest(proxy.GetPort(), "FlapRecover");
            return b1->GetCircuitState() == CircuitState::Closed;
        }, 1500ms);
        ASSERT_TRUE(closed);

        // Stop restarter for next cycle
        restarter.Stop();
    }

    StopProxy();
    EXPECT_EQ(proxy.GetActiveConnectionCount(), 0u);
}

// 4. Resource safety on circuit open with zero leaks
TEST_F(CircuitBreakerIntegrationTest, ResourceSafetyOnCircuitOpen) {
    TestTcpBackend backend1;
    ASSERT_TRUE(backend1.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    L4ProxyServer proxy(config);

    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", backend1.GetPort());
    CircuitBreakerConfig cb_config;
    cb_config.failure_threshold = 1;
    b1->ConfigureCircuitBreaker(cb_config);
    proxy.GetLoadBalancer().AddBackend(b1);

    ASSERT_TRUE(proxy.Start());
    StartProxy(proxy);

    // Initial normal echo
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "ResourceSafeTest"));

    // Stop backend
    backend1.Stop();

    // Trigger failure
    SendEchoRequest(proxy.GetPort(), "FailureReq", 50ms);

    // Circuit is Open
    bool is_open = WaitFor([&]() {
        return b1->GetCircuitState() == CircuitState::Open;
    }, 1500ms);
    EXPECT_TRUE(is_open);

    // Verify zero connection leaks and clean buffer pool
    StopProxy();
    EXPECT_FALSE(proxy.IsRunning());
    EXPECT_EQ(proxy.GetActiveConnectionCount(), 0u);
    EXPECT_EQ(b1->GetActiveConnections(), 0u);
    EXPECT_EQ(proxy.GetBufferPool().CheckedOutCount(), 0u);
    EXPECT_EQ(proxy.GetBufferPool().FreeCount(), proxy.GetBufferPool().TotalBuffers());
}

// 5. Probe limit enforcement prevents concurrent probe flooding in HalfOpen
TEST_F(CircuitBreakerIntegrationTest, ProbeLimitConcurrentTrafficProtection) {
    TestTcpBackend backend1;
    TestTcpBackend backend2;
    ASSERT_TRUE(backend1.Start());
    ASSERT_TRUE(backend2.Start());
    uint16_t port1 = backend1.GetPort();

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.lb_algorithm = "round_robin";
    L4ProxyServer proxy(config);

    auto b1 = std::make_shared<Backend>(1, "127.0.0.1", port1);
    auto b2 = std::make_shared<Backend>(2, "127.0.0.1", backend2.GetPort());

    CircuitBreakerConfig cb_config;
    cb_config.failure_threshold = 1;
    cb_config.cooldown = 100ms;
    cb_config.half_open_max_probes = 1; // Strict limit: 1 probe
    b1->ConfigureCircuitBreaker(cb_config);
    b2->ConfigureCircuitBreaker(cb_config);

    proxy.GetLoadBalancer().AddBackend(b1);
    proxy.GetLoadBalancer().AddBackend(b2);

    ASSERT_TRUE(proxy.Start());
    StartProxy(proxy);

    // Trip backend 1
    b1->GetCircuitBreaker().Trip();
    EXPECT_EQ(b1->GetCircuitState(), CircuitState::Open);

    // Wait for cooldown to expire -> HalfOpen
    bool half_open = WaitFor([&]() {
        return b1->GetCircuitState() == CircuitState::HalfOpen;
    }, 1500ms);
    ASSERT_TRUE(half_open);

    // Client 1 connects and holds connection open (occupies the 1 probe slot)
    int client1_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(client1_fd, 0);
    sockaddr_in addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &addr));
    ASSERT_EQ(connect(client1_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    // Send probe ping on client 1
    std::string ping1 = "ProbePing1";
    ASSERT_EQ(send(client1_fd, ping1.data(), ping1.size(), 0), static_cast<ssize_t>(ping1.size()));
    char buf1[64]{};
    ASSERT_EQ(recv(client1_fd, buf1, sizeof(buf1), 0), static_cast<ssize_t>(ping1.size()));

    // While client 1 is connected, send request with client 2
    // Backend 1 has exhausted its probe capacity (1), so client 2 MUST go to backend 2
    backend2.ResetEchoCount();
    EXPECT_TRUE(SendEchoRequest(proxy.GetPort(), "Client2ReqToB2"));
    EXPECT_GE(backend2.GetEchoCount(), 1u);

    // Close client 1
    ::close(client1_fd);

    // Wait for b1 active connections to drain
    WaitFor([&]() {
        return b1->GetActiveConnections() == 0u;
    }, 1500ms);

    StopProxy();
    EXPECT_EQ(proxy.GetActiveConnectionCount(), 0u);
}
