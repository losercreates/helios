#include <gtest/gtest.h>
#include "net/l4_proxy_server.hpp"
#include "metrics/metrics_collector.hpp"
#include "../common/test_tcp_backend.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <string>
#include <memory>

using namespace helios;

class SecurityIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        stop_proxy_ = false;
    }

    void TearDown() override {
        StopProxyLoop();
    }

    void StartProxyLoop(L4ProxyServer& proxy) {
        stop_proxy_ = false;
        proxy_thread_ = std::thread([&proxy, this]() {
            while (!stop_proxy_) {
                proxy.RunOnce(std::chrono::milliseconds(1));
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            proxy.Stop();
        });
    }

    void StopProxyLoop() {
        if (proxy_thread_.joinable()) {
            stop_proxy_ = true;
            proxy_thread_.join();
        }
    }

    int ConnectToProxy(uint16_t port) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GT(fd, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(fd);
            return -1;
        }
        return fd;
    }

    std::atomic<bool> stop_proxy_{false};
    std::thread proxy_thread_;
};

// 1. ACL Blocks Forbidden Client On Accept
TEST_F(SecurityIntegrationTest, AclBlocksForbiddenClientOnAccept) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    auto metrics = std::make_shared<WorkerMetrics>(0);

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.security.acl.enabled = true;
    config.security.acl.default_policy = "allow";
    config.security.acl.rules.push_back({"127.0.0.1/32", "deny"});

    L4ProxyServer proxy(config);
    proxy.SetMetrics(metrics);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    int client_fd = ConnectToProxy(proxy.GetPort());
    ASSERT_GT(client_fd, 0);

    // Send a payload
    const std::string payload = "Hello World";
    send(client_fd, payload.data(), payload.size(), 0);

    // Because ACL denied 127.0.0.1, proxy closes socket immediately.
    // Reading should return EOF (0) or error.
    char buf[128];
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    EXPECT_LE(n, 0);
    close(client_fd);

    // Wait briefly for metrics to register
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(backend.GetAcceptedCount(), 0u);
    EXPECT_GE(metrics->acl_rejected_total.load(), 1u);

    StopProxyLoop();
}

// 2. ACL Allows Permitted Client
TEST_F(SecurityIntegrationTest, AclAllowsPermittedClient) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    auto metrics = std::make_shared<WorkerMetrics>(0);

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.security.acl.enabled = true;
    config.security.acl.default_policy = "deny";
    config.security.acl.rules.push_back({"127.0.0.1/32", "allow"});

    L4ProxyServer proxy(config);
    proxy.SetMetrics(metrics);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    int client_fd = ConnectToProxy(proxy.GetPort());
    ASSERT_GT(client_fd, 0);

    const std::string payload = "Permitted Traffic";
    send(client_fd, payload.data(), payload.size(), 0);

    char buf[128];
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    EXPECT_GT(n, 0);
    std::string resp(buf, static_cast<size_t>(n));
    EXPECT_EQ(resp, payload);
    close(client_fd);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(backend.GetAcceptedCount(), 1u);
    EXPECT_EQ(metrics->acl_rejected_total.load(), 0u);

    StopProxyLoop();
}

// 3. L4 Transport Rate Limiting Drops Excess Connections
TEST_F(SecurityIntegrationTest, L4TransportRateLimitingDropsExcessConnections) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    auto metrics = std::make_shared<WorkerMetrics>(0);

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_l7_routing = false; // Pure L4
    config.security.rate_limit.enabled = true;
    config.security.rate_limit.rate_per_sec = 1.0;
    config.security.rate_limit.burst_size = 2;
    config.security.rate_limit.cidr_mask = 32;

    L4ProxyServer proxy(config);
    proxy.SetMetrics(metrics);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    // Connection 1 (Burst 1) -> Allowed
    int fd1 = ConnectToProxy(proxy.GetPort());
    ASSERT_GT(fd1, 0);
    send(fd1, "ping1", 5, 0);

    // Connection 2 (Burst 2) -> Allowed
    int fd2 = ConnectToProxy(proxy.GetPort());
    ASSERT_GT(fd2, 0);
    send(fd2, "ping2", 5, 0);

    // Connection 3 -> Exceeds burst capacity!
    int fd3 = ConnectToProxy(proxy.GetPort());
    ASSERT_GT(fd3, 0);
    send(fd3, "ping3", 5, 0);

    // fd3 should be closed by proxy immediately
    char buf[64];
    ssize_t n3 = recv(fd3, buf, sizeof(buf), 0);
    EXPECT_LE(n3, 0);

    close(fd1);
    close(fd2);
    close(fd3);

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_GE(metrics->rate_limit_rejected_total.load(), 1u);

    StopProxyLoop();
}

// 4. L7 Rate Limiting Returns 429 Too Many Requests
TEST_F(SecurityIntegrationTest, L7RateLimitingReturns429TooManyRequests) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    auto metrics = std::make_shared<WorkerMetrics>(0);

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_l7_routing = true;
    config.security.rate_limit.enabled = true;
    config.security.rate_limit.rate_per_sec = 2.0;
    config.security.rate_limit.burst_size = 2;
    config.security.rate_limit.cidr_mask = 32;

    L4ProxyServer proxy(config);
    proxy.SetMetrics(metrics);
    proxy.AddNamedPool("default_pool", "round_robin");
    proxy.AddBackendToNamedPool("default_pool", "127.0.0.1", backend.GetPort());
    proxy.SetDefaultPool("default_pool");
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    auto send_req = [&](const std::string& path) -> std::string {
        int fd = ConnectToProxy(proxy.GetPort());
        if (fd < 0) return "";
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        send(fd, req.data(), req.size(), 0);
        shutdown(fd, SHUT_WR);

        std::string resp;
        char buf[512];
        while (true) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            resp.append(buf, static_cast<size_t>(n));
            if (resp.find("\r\n\r\n") != std::string::npos) break;
        }
        close(fd);
        return resp;
    };

    // Request 1 -> OK (echoed by backend)
    std::string resp1 = send_req("/api/test1");
    EXPECT_NE(resp1.find("GET /api/test1 HTTP/1.1"), std::string::npos);

    // Request 2 -> OK
    std::string resp2 = send_req("/api/test2");
    EXPECT_NE(resp2.find("GET /api/test2 HTTP/1.1"), std::string::npos);

    // Request 3 -> Rate limit exceeded! Should return HTTP 429
    std::string resp3 = send_req("/api/test3");
    EXPECT_NE(resp3.find("429 Too Many Requests"), std::string::npos);
    EXPECT_NE(resp3.find("Retry-After: 1"), std::string::npos);

    EXPECT_GE(metrics->rate_limit_rejected_total.load(), 1u);

    StopProxyLoop();
}

// 5. Slowloris Header Timeout Returns 408 Request Timeout
TEST_F(SecurityIntegrationTest, SlowlorisHeaderTimeoutReturns408RequestTimeout) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    auto metrics = std::make_shared<WorkerMetrics>(0);

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_l7_routing = true;
    config.timeout_check_interval = std::chrono::milliseconds(50);
    config.header_read_timeout = std::chrono::milliseconds(150); // 150ms timeout

    L4ProxyServer proxy(config);
    proxy.SetMetrics(metrics);
    proxy.AddNamedPool("default_pool", "round_robin");
    proxy.AddBackendToNamedPool("default_pool", "127.0.0.1", backend.GetPort());
    proxy.SetDefaultPool("default_pool");
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    int client_fd = ConnectToProxy(proxy.GetPort());
    ASSERT_GT(client_fd, 0);

    // Send incomplete partial headers (Slowloris drip)
    const std::string partial_req = "GET /slow-header HTTP/1.1\r\nHost: localhost\r\n";
    send(client_fd, partial_req.data(), partial_req.size(), 0);

    // Wait ~250ms for header_read_timeout (150ms) to trigger in CheckTimeouts
    std::string resp;
    char buf[512];
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() < 1000) {
        ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
        if (resp.find("408 Request Timeout") != std::string::npos) break;
    }
    close(client_fd);

    EXPECT_NE(resp.find("HTTP/1.1 408 Request Timeout"), std::string::npos);
    EXPECT_GE(metrics->slowloris_rejected_total.load(), 1u);

    StopProxyLoop();
}
