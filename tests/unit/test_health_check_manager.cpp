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

class MockHttpServer {
public:
    explicit MockHttpServer(int status_code = 200) : status_code_(status_code) {}
    ~MockHttpServer() { Stop(); }

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
        addr.sin_port = 0; // Ephemeral

        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd);
            return false;
        }

        socklen_t len = sizeof(addr);
        getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);

        struct timeval tv{0, 20000}; // 20ms accept timeout
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (listen(fd, 64) < 0) {
            close(fd);
            return false;
        }

        listen_fd_.store(fd);
        running_.store(true);
        thread_ = std::thread([this]() {
            while (running_.load()) {
                int lfd = listen_fd_.load();
                if (lfd < 0) break;
                sockaddr_in client_addr{};
                socklen_t clen = sizeof(client_addr);
                int client_fd = accept(lfd, reinterpret_cast<sockaddr*>(&client_addr), &clen);
                if (client_fd < 0) continue;

                char buf[1024];
                ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
                (void)n;

                int code = status_code_.load();
                std::string resp;
                if (code == 200) {
                    resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
                } else if (code == 204) {
                    resp = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
                } else {
                    resp = "HTTP/1.1 " + std::to_string(code) + " Error\r\nContent-Length: 5\r\nConnection: close\r\n\r\nError";
                }
                send(client_fd, resp.data(), resp.size(), 0);
                close(client_fd);
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
            if (thread_.joinable()) {
                thread_.join();
            }
        }
    }

    void SetStatusCode(int code) { status_code_.store(code); }
    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }

private:
    std::atomic<int> listen_fd_{-1};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::atomic<int> status_code_{200};
    std::thread thread_;
};

TEST(HealthCheckManagerTest, HttpProbingSuccessMaintainsHealthy) {
    MockHttpServer server(200);
    ASSERT_TRUE(server.Start());

    HealthCheckConfig cfg;
    cfg.enabled = true;
    cfg.type = "http";
    cfg.path = "/health";
    cfg.expected_status = 200;
    cfg.interval = 30ms;
    cfg.timeout = 50ms;
    cfg.unhealthy_threshold = 2;
    cfg.healthy_threshold = 1;

    HealthCheckManager manager(cfg);
    auto backend = std::make_shared<Backend>(20, "127.0.0.1", server.GetPort(), 1);
    manager.RegisterBackend(backend);

    ASSERT_TRUE(manager.Start());
    EXPECT_TRUE(WaitFor([&]() { return backend->GetHealthState() == HealthState::Healthy; }));

    // Remain healthy with 200 OK responses
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(backend->GetHealthState(), HealthState::Healthy);
    EXPECT_TRUE(backend->IsEligible());

    manager.Stop();
}

TEST(HealthCheckManagerTest, HttpProbingMismatchTransitionsToUnhealthy) {
    MockHttpServer server(500);
    ASSERT_TRUE(server.Start());

    HealthCheckConfig cfg;
    cfg.enabled = true;
    cfg.type = "http";
    cfg.path = "/health";
    cfg.expected_status = 200;
    cfg.interval = 25ms;
    cfg.timeout = 50ms;
    cfg.unhealthy_threshold = 2;
    cfg.healthy_threshold = 1;

    HealthCheckManager manager(cfg);
    auto backend = std::make_shared<Backend>(21, "127.0.0.1", server.GetPort(), 1);
    manager.RegisterBackend(backend);

    ASSERT_TRUE(manager.Start());

    // 500 error fails expected 200 -> transitions to Unhealthy
    EXPECT_TRUE(WaitFor([&]() { return backend->GetHealthState() == HealthState::Unhealthy; }, 2000ms));
    EXPECT_FALSE(backend->IsEligible());

    // Recover by switching server to 200
    server.SetStatusCode(200);
    EXPECT_TRUE(WaitFor([&]() { return backend->GetHealthState() == HealthState::Healthy; }, 2000ms));
    EXPECT_TRUE(backend->IsEligible());

    manager.Stop();
}

TEST(HealthCheckManagerTest, HttpProbingExpectedStatusZeroAcceptsAny2xx) {
    MockHttpServer server(204);
    ASSERT_TRUE(server.Start());

    HealthCheckConfig cfg;
    cfg.enabled = true;
    cfg.type = "http";
    cfg.path = "/status";
    cfg.expected_status = 0; // any 2xx
    cfg.interval = 25ms;
    cfg.timeout = 50ms;

    HealthCheckManager manager(cfg);
    auto backend = std::make_shared<Backend>(22, "127.0.0.1", server.GetPort(), 1);
    manager.RegisterBackend(backend);

    ASSERT_TRUE(manager.Start());
    EXPECT_TRUE(WaitFor([&]() { return backend->GetHealthState() == HealthState::Healthy; }, 1000ms));

    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(backend->GetHealthState(), HealthState::Healthy);

    manager.Stop();
}

TEST(HealthCheckManagerTest, PrometheusTelemetryMetricsExposed) {
    MockHttpServer server(200);
    ASSERT_TRUE(server.Start());

    auto metrics = std::make_shared<MetricsCollector>();

    HealthCheckConfig cfg;
    cfg.enabled = true;
    cfg.type = "http";
    cfg.path = "/health";
    cfg.expected_status = 200;
    cfg.interval = 20ms;
    cfg.timeout = 50ms;

    HealthCheckManager manager(cfg);
    manager.SetMetricsCollector(metrics);

    auto backend = std::make_shared<Backend>(30, "127.0.0.1", server.GetPort(), 1);
    metrics->RecordBackendHealth("127.0.0.1:" + std::to_string(server.GetPort()), "primary_pool", 1);
    manager.RegisterBackend(backend);

    ASSERT_TRUE(manager.Start());

    // Wait for probes to execute
    EXPECT_TRUE(WaitFor([&]() {
        return metrics->GetHealthCheckProbesTotal("127.0.0.1:" + std::to_string(server.GetPort()), "success") > 0;
    }, 2000ms));

    std::string text = metrics->RenderPrometheusText();
    EXPECT_NE(text.find("helios_backend_health_status{endpoint=\"127.0.0.1:" + std::to_string(server.GetPort()) + "\",pool=\"primary_pool\"} 1"), std::string::npos);
    EXPECT_NE(text.find("helios_health_check_probes_total{endpoint=\"127.0.0.1:" + std::to_string(server.GetPort()) + "\",result=\"success\"}"), std::string::npos);

    manager.Stop();
}

TEST(HealthCheckManagerTest, UnhealthyTransitionInvokesCallback) {
    MockHttpServer server(503);
    ASSERT_TRUE(server.Start());

    HealthCheckConfig cfg;
    cfg.enabled = true;
    cfg.type = "http";
    cfg.path = "/health";
    cfg.expected_status = 200;
    cfg.interval = 20ms;
    cfg.timeout = 50ms;
    cfg.unhealthy_threshold = 2;

    HealthCheckManager manager(cfg);
    std::atomic<bool> callback_fired{false};
    std::atomic<uint32_t> evicted_backend_id{0};

    manager.SetOnBackendUnhealthyCallback([&](uint32_t b_id) {
        callback_fired.store(true);
        evicted_backend_id.store(b_id);
    });

    auto backend = std::make_shared<Backend>(77, "127.0.0.1", server.GetPort(), 1);
    manager.RegisterBackend(backend);

    ASSERT_TRUE(manager.Start());

    EXPECT_TRUE(WaitFor([&]() { return callback_fired.load(); }, 2000ms));
    EXPECT_EQ(evicted_backend_id.load(), 77u);

    manager.Stop();
}

