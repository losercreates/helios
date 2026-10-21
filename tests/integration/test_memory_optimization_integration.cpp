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
#include <vector>
#include <memory>

using namespace helios;

class MemoryOptimizationIntegrationTest : public ::testing::Test {
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

// 1. Idle Connections Consume Zero Buffer Pool Memory
TEST_F(MemoryOptimizationIntegrationTest, IdleConnectionsConsumeZeroBuffers) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    auto metrics = std::make_shared<WorkerMetrics>(0);

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_l7_routing = true;
    config.enable_lazy_buffer_allocation = true;
    config.total_buffers = 32;
    config.buffer_size = 16384;

    L4ProxyServer proxy(config);
    proxy.SetMetrics(metrics);
    proxy.AddNamedPool("default_pool", "round_robin");
    proxy.AddBackendToNamedPool("default_pool", "127.0.0.1", backend.GetPort());
    proxy.SetDefaultPool("default_pool");
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    // Open 8 concurrent idle client connections without sending any payload
    std::vector<int> idle_fds;
    for (int i = 0; i < 8; ++i) {
        int fd = ConnectToProxy(proxy.GetPort());
        ASSERT_GT(fd, 0);
        idle_fds.push_back(fd);
    }

    // Wait briefly for all 8 connections to be accepted
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // CRITICAL ASSERTION: All 8 connections are idle waiting for incoming requests;
    // thanks to poll-first lazy buffer allocation, ZERO userspace buffers are checked out!
    EXPECT_EQ(proxy.GetBufferPool().CheckedOutCount(), 0u);
    EXPECT_EQ(proxy.GetBufferPool().ActiveAllocatedBytes(), 0u);

    // Now send a request on the first connection
    std::string req = "GET /test-lazy HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send(idle_fds[0], req.data(), req.size(), 0);

    // Read response
    char buf[512];
    std::string resp;
    while (true) {
        ssize_t n = recv(idle_fds[0], buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
        if (resp.find("GET /test-lazy") != std::string::npos) break;
    }
    EXPECT_NE(resp.find("GET /test-lazy HTTP/1.1"), std::string::npos);

    close(idle_fds[0]);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Buffer is released back to pool
    EXPECT_EQ(proxy.GetBufferPool().CheckedOutCount(), 0u);

    // Close remaining idle connections
    for (size_t i = 1; i < idle_fds.size(); ++i) {
        close(idle_fds[i]);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    StopProxyLoop();
}

// 2. Slab Pool Recycles Connection State
TEST_F(MemoryOptimizationIntegrationTest, SlabPoolRecyclesConnectionState) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_l7_routing = true;

    L4ProxyServer proxy(config);
    proxy.AddNamedPool("default_pool", "round_robin");
    proxy.AddBackendToNamedPool("default_pool", "127.0.0.1", backend.GetPort());
    proxy.SetDefaultPool("default_pool");
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    for (int i = 0; i < 10; ++i) {
        int fd = ConnectToProxy(proxy.GetPort());
        ASSERT_GT(fd, 0);
        std::string req = "GET /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        send(fd, req.data(), req.size(), 0);

        char buf[256];
        recv(fd, buf, sizeof(buf), 0);
        close(fd);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    StopProxyLoop();

    // Slab pool should have recycled all closed connection slots
    EXPECT_EQ(proxy.GetSlabPool().ActiveCount(), 0u);
}

// 3. Prometheus Telemetry Reflects Memory Gauges
TEST_F(MemoryOptimizationIntegrationTest, PrometheusTelemetryReflectsMemoryGauges) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    auto metrics = std::make_shared<WorkerMetrics>(0);

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.total_buffers = 16;
    config.buffer_size = 16384;

    L4ProxyServer proxy(config);
    proxy.SetMetrics(metrics);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    MetricsCollector collector;
    collector.RegisterWorkerMetrics(metrics);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::string text = collector.RenderPrometheusText();
    EXPECT_NE(text.find("helios_process_resident_memory_bytes"), std::string::npos);
    EXPECT_NE(text.find("helios_process_virtual_memory_bytes"), std::string::npos);
    EXPECT_NE(text.find("helios_buffer_pool_bytes_allocated{worker=\"0\"} 262144"), std::string::npos);

    StopProxyLoop();
}
