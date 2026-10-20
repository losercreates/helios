#include <gtest/gtest.h>
#include "net/l4_proxy_server.hpp"
#include "net/socket_utils.hpp"

#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <chrono>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace helios;

#include "../common/test_tcp_backend.hpp"

class L4ProxyIntegrationTest : public ::testing::Test {
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

    std::atomic<bool> stop_proxy_{false};
    std::thread proxy_thread_;
};

// 1. Basic Request/Response Byte Forwarding
TEST_F(L4ProxyIntegrationTest, BasicByteForwarding) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0; // Ephemeral
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(client_fd, 0);

    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &target_addr));
    ASSERT_EQ(connect(client_fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    std::string request = "Hello Helios L4 TCP Proxy!";
    send(client_fd, request.data(), request.size(), 0);

    std::array<char, 256> response{};
    ssize_t n = recv(client_fd, response.data(), response.size(), 0);
    ASSERT_EQ(n, static_cast<ssize_t>(request.size()));
    EXPECT_EQ(std::string(response.data(), static_cast<size_t>(n)), request);

    close(client_fd);
    StopProxyLoop();
}

// 2. Large Payload Data Integrity
TEST_F(L4ProxyIntegrationTest, LargePayloadForwardingDataIntegrity) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.buffer_size = 4096;
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(client_fd, 0);

    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &target_addr));
    ASSERT_EQ(connect(client_fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    // 256 KB random payload
    std::vector<uint8_t> send_data(256 * 1024);
    for (size_t i = 0; i < send_data.size(); ++i) {
        send_data[i] = static_cast<uint8_t>(i % 256);
    }

    std::thread sender([client_fd, &send_data]() {
        size_t total_sent = 0;
        while (total_sent < send_data.size()) {
            size_t chunk = std::min<size_t>(16384, send_data.size() - total_sent);
            ssize_t n = send(client_fd, send_data.data() + total_sent, chunk, 0);
            if (n <= 0) break;
            total_sent += static_cast<size_t>(n);
        }
    });

    std::vector<uint8_t> recv_data(send_data.size(), 0);
    size_t total_recvd = 0;
    while (total_recvd < send_data.size()) {
        ssize_t n = recv(client_fd, recv_data.data() + total_recvd, send_data.size() - total_recvd, 0);
        if (n <= 0) break;
        total_recvd += static_cast<size_t>(n);
    }

    sender.join();
    EXPECT_EQ(total_recvd, send_data.size());
    EXPECT_EQ(recv_data, send_data);

    close(client_fd);
    StopProxyLoop();
}

// 3. Fragmented Payload Forwarding
TEST_F(L4ProxyIntegrationTest, FragmentedPayloadForwarding) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(client_fd, 0);

    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &target_addr));
    ASSERT_EQ(connect(client_fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    std::string full_msg = "FragmentedDataStreamPayload";
    std::string received_msg;

    for (char c : full_msg) {
        send(client_fd, &c, 1, 0);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        char r;
        if (recv(client_fd, &r, 1, 0) == 1) {
            received_msg.push_back(r);
        }
    }

    EXPECT_EQ(received_msg, full_msg);

    close(client_fd);
    StopProxyLoop();
}

// 4. Multiple Simultaneous Clients & Round-Robin Load Balancing
TEST_F(L4ProxyIntegrationTest, MultipleClientsRoundRobinDistribution) {
    TestTcpBackend backend1;
    TestTcpBackend backend2;
    ASSERT_TRUE(backend1.Start());
    ASSERT_TRUE(backend2.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend1.GetPort());
    proxy.AddBackend("127.0.0.1", backend2.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    constexpr int kNumClients = 6;
    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &target_addr));

    for (int i = 0; i < kNumClients; ++i) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GT(fd, 0);
        ASSERT_EQ(connect(fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

        std::string ping = "Ping" + std::to_string(i);
        send(fd, ping.data(), ping.size(), 0);

        char buf[32]{};
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        ASSERT_GT(n, 0);
        EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), ping);

        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(backend1.GetAcceptedCount(), 3u);
    EXPECT_EQ(backend2.GetAcceptedCount(), 3u);

    StopProxyLoop();
}

// 5. Backend Connection Refused (Unavailable Backend)
TEST_F(L4ProxyIntegrationTest, BackendUnavailableRefusedConnection) {
    L4ProxyServer::Config config;
    config.listen_port = 0;
    L4ProxyServer proxy(config);
    // Unassigned port 59999
    proxy.AddBackend("127.0.0.1", 59999);
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(client_fd, 0);

    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &target_addr));
    ASSERT_EQ(connect(client_fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    // Send payload; proxy attempts connect to 59999, fails, and closes client connection
    std::string ping = "Ping";
    send(client_fd, ping.data(), ping.size(), 0);

    char buf[16]{};
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    EXPECT_LE(n, 0); // Clean EOF or connection reset from proxy

    close(client_fd);
    StopProxyLoop();
}

// 6. Client Half-Close (FIN) Handling
TEST_F(L4ProxyIntegrationTest, ClientHalfClose) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(client_fd, 0);

    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &target_addr));
    ASSERT_EQ(connect(client_fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    std::string payload = "HalfCloseRequest";
    send(client_fd, payload.data(), payload.size(), 0);

    // Send FIN from client
    shutdown(client_fd, SHUT_WR);

    char buf[64]{};
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    EXPECT_EQ(n, static_cast<ssize_t>(payload.size()));
    if (n > 0) {
        EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), payload);
    }

    close(client_fd);
    StopProxyLoop();
}

// 7. Buffer Exhaustion & Recovery under Backpressure
TEST_F(L4ProxyIntegrationTest, BufferExhaustionAndRecovery) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.total_buffers = 4; // Constrained tiny buffer pool
    config.buffer_size = 1024;
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(client_fd, 0);

    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &target_addr));
    ASSERT_EQ(connect(client_fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    std::string msg = "BufferExhaustionRecoveryTest";
    send(client_fd, msg.data(), msg.size(), 0);

    char buf[64]{};
    ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
    ASSERT_EQ(n, static_cast<ssize_t>(msg.size()));
    if (n > 0) {
        EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), msg);
    }

    close(client_fd);
    StopProxyLoop();
}

// 8. Graceful Proxy Shutdown with Active Connections
TEST_F(L4ProxyIntegrationTest, GracefulShutdownWithActiveConnections) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());

    int client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(client_fd, 0);

    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &target_addr));
    ASSERT_EQ(connect(client_fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    proxy.RunOnce(std::chrono::milliseconds(10));

    // Gracefully stop proxy
    proxy.Stop();

    // Verify proxy state
    EXPECT_FALSE(proxy.IsRunning());
    EXPECT_EQ(proxy.GetActiveConnectionCount(), 0u);

    close(client_fd);
}

// 9. Weighted Round-Robin Load Balancing
TEST_F(L4ProxyIntegrationTest, WeightedRoundRobinDistribution) {
    TestTcpBackend backend1;
    TestTcpBackend backend2;
    ASSERT_TRUE(backend1.Start());
    ASSERT_TRUE(backend2.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.lb_algorithm = "weighted_round_robin";
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend1.GetPort(), 1);
    proxy.AddBackend("127.0.0.1", backend2.GetPort(), 3);
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    constexpr int kNumClients = 20;
    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &target_addr));

    for (int i = 0; i < kNumClients; ++i) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GT(fd, 0);
        ASSERT_EQ(connect(fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

        std::string ping = "Ping" + std::to_string(i);
        send(fd, ping.data(), ping.size(), 0);

        char buf[32]{};
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        ASSERT_GT(n, 0);
        EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), ping);

        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(backend1.GetAcceptedCount(), 5u);
    EXPECT_EQ(backend2.GetAcceptedCount(), 15u);

    StopProxyLoop();
}

// 10. Least-Connections Load Balancing
TEST_F(L4ProxyIntegrationTest, LeastConnectionsDistribution) {
    TestTcpBackend backend1;
    TestTcpBackend backend2;
    ASSERT_TRUE(backend1.Start());
    ASSERT_TRUE(backend2.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.lb_algorithm = "least_connections";
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend1.GetPort());
    proxy.AddBackend("127.0.0.1", backend2.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &target_addr));

    // Connect Client A, it should go to backend1 (tie break by ID)
    int fdA = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(fdA, 0);
    ASSERT_EQ(connect(fdA, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    // Give proxy a moment to register connection state
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Connect Client B, since backend1 has 1 connection and backend2 has 0, it should go to backend2
    int fdB = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(fdB, 0);
    ASSERT_EQ(connect(fdB, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Send payload to check routing
    std::string pingA = "PingA";
    send(fdA, pingA.data(), pingA.size(), 0);
    
    std::string pingB = "PingB";
    send(fdB, pingB.data(), pingB.size(), 0);

    char buf[32]{};
    recv(fdA, buf, sizeof(buf), 0);
    recv(fdB, buf, sizeof(buf), 0);
    
    // Check that each backend got exactly 1 connection so far
    EXPECT_EQ(backend1.GetAcceptedCount(), 1u);
    EXPECT_EQ(backend2.GetAcceptedCount(), 1u);

    // Connect Client C. Both have 1 connection, tie-break gives it to backend1.
    int fdC = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(fdC, 0);
    ASSERT_EQ(connect(fdC, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(backend1.GetAcceptedCount(), 2u);
    EXPECT_EQ(backend2.GetAcceptedCount(), 1u);

    // Disconnect Client A and C, dropping backend1's connections to 0.
    close(fdA);
    close(fdC);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Connect Client D. backend1 has 0, backend2 has 1. Should go to backend1.
    int fdD = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(fdD, 0);
    ASSERT_EQ(connect(fdD, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(backend1.GetAcceptedCount(), 3u); // Total historically accepted
    EXPECT_EQ(backend2.GetAcceptedCount(), 1u);

    close(fdB);
    close(fdD);

    StopProxyLoop();
}

// 11. Backend Draining Lifecycle Validation
TEST_F(L4ProxyIntegrationTest, BackendDrainingStateAndConnectionLifecycle) {
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

    // Connect Client 1 -> Should go to b1 (round robin)
    int fd1 = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(fd1, 0);
    ASSERT_EQ(connect(fd1, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(backend1.GetAcceptedCount(), 1u);
    EXPECT_EQ(b1->GetActiveConnections(), 1u);

    // Now mark b1 as draining
    b1->SetDraining(true);

    // Existing connection on b1 should still work (verify data flows)
    std::string ping = "StillAlive";
    send(fd1, ping.data(), ping.size(), 0);
    char buf[32]{};
    recv(fd1, buf, sizeof(buf), 0);
    EXPECT_EQ(b1->GetActiveConnections(), 1u); // Should still have the active connection

    // New connection -> Must skip draining b1 and go to b2
    int fd2 = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(fd2, 0);
    ASSERT_EQ(connect(fd2, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(backend2.GetAcceptedCount(), 1u);
    EXPECT_EQ(b2->GetActiveConnections(), 1u);
    
    // Another new connection -> Should still go to b2, never b1
    int fd3 = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(fd3, 0);
    ASSERT_EQ(connect(fd3, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(backend2.GetAcceptedCount(), 2u);
    EXPECT_EQ(b2->GetActiveConnections(), 2u);
    EXPECT_EQ(backend1.GetAcceptedCount(), 1u); // Did not increase

    // Now close client 1 (which terminates the connection to draining b1)
    close(fd1);
    
    // Allow io_uring to process the close
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // b1 should now safely hit 0 active connections while draining
    EXPECT_EQ(b1->GetActiveConnections(), 0u);
    EXPECT_TRUE(b1->IsDraining());
    
    // Once active connections are 0, we can safely remove it from the LB
    EXPECT_TRUE(proxy.GetLoadBalancer().RemoveBackend(1));

    close(fd2);
    close(fd3);
    StopProxyLoop();
}

// 12. Backend Health Lifecycle Validation
TEST_F(L4ProxyIntegrationTest, BackendHealthTransitionsAndTrafficRouting) {
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
    
    b1->SetHealthThresholds(2, 2); // 2 fails -> unhealthy, 2 success -> healthy
    b2->SetHealthThresholds(2, 2);

    proxy.GetLoadBalancer().AddBackend(b1);
    proxy.GetLoadBalancer().AddBackend(b2);

    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy.GetPort(), &target_addr));

    // Connect Client 1 -> Should go to b1 (round robin)
    int fd1 = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_EQ(connect(fd1, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(b1->GetActiveConnections(), 1u);
    EXPECT_EQ(b2->GetActiveConnections(), 0u);

    // Fail b1
    b1->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    EXPECT_EQ(b1->GetHealthState(), HealthState::Healthy); // still healthy
    EXPECT_TRUE(b1->IsEligible());

    b1->ReportHealthCheckResult(HealthCheckResult::ConnectionFailure);
    EXPECT_EQ(b1->GetHealthState(), HealthState::Unhealthy); // now unhealthy
    EXPECT_FALSE(b1->IsEligible());

    // Connect Client 2 -> Should go to b2, bypassing b1
    int fd2 = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_EQ(connect(fd2, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(b2->GetActiveConnections(), 1u); // routed to b2
    
    // Existing connection on b1 should still work
    std::string ping = "Ping";
    send(fd1, ping.data(), ping.size(), 0);
    char buf[32]{};
    recv(fd1, buf, sizeof(buf), 0);
    EXPECT_EQ(b1->GetActiveConnections(), 1u); // b1 didn't drop the connection

    // Recover b1
    b1->ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(b1->GetHealthState(), HealthState::Recovering);
    EXPECT_FALSE(b1->IsEligible()); // Still ineligible

    // Client 3 -> Should still go to b2
    int fd3 = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_EQ(connect(fd3, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(b2->GetActiveConnections(), 2u);

    // Final recovery success
    b1->ReportHealthCheckResult(HealthCheckResult::Success);
    EXPECT_EQ(b1->GetHealthState(), HealthState::Healthy);
    EXPECT_TRUE(b1->IsEligible());

    // Client 4 -> Should go to b1 now since it's eligible and it's b1's turn in round-robin
    int fd4 = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_EQ(connect(fd4, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    
    EXPECT_EQ(b1->GetActiveConnections(), 2u); // 1 old + 1 new
    
    close(fd1);
    close(fd2);
    close(fd3);
    close(fd4);
    StopProxyLoop();
}

// 13. Backend Connection Pool Reuses Idle Connections Across Sequential Requests
TEST_F(L4ProxyIntegrationTest, BackendConnectionPoolReusesIdleConnections) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_connection_pooling = true;
    config.connection_pool_config.idle_timeout = std::chrono::milliseconds(5000);
    config.connection_pool_config.max_idle_per_backend = 10;

    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());

    auto metrics = std::make_shared<WorkerMetrics>(0);
    proxy.SetMetrics(metrics);

    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    sockaddr_in target_addr{};
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(proxy.GetPort());
    inet_pton(AF_INET, "127.0.0.1", &target_addr.sin_addr);

    // Request 1: Fresh backend connection (pool miss)
    int fd1 = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_EQ(connect(fd1, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    std::string req1 = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nHello";
    send(fd1, req1.data(), req1.size(), 0);

    char buf[128]{};
    ssize_t n1 = recv(fd1, buf, sizeof(buf) - 1, 0);
    ASSERT_GT(n1, 0);
    close(fd1);

    // Allow proxy loop to complete cleanup and recycle backend socket into pool
    for (int retry = 0; retry < 50; ++retry) {
        if (proxy.GetBackendPool() && proxy.GetBackendPool()->TotalIdleCount() > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_NE(proxy.GetBackendPool(), nullptr);
    EXPECT_EQ(proxy.GetBackendPool()->TotalIdleCount(), 1u);
    EXPECT_EQ(metrics->backend_pool_misses_total.load(), 1u);
    EXPECT_EQ(metrics->backend_pool_hits_total.load(), 0u);

    // Request 2: Reuses the pooled backend connection (pool hit)
    int fd2 = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_EQ(connect(fd2, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    std::string req2 = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nWorld";
    send(fd2, req2.data(), req2.size(), 0);

    std::memset(buf, 0, sizeof(buf));
    ssize_t n2 = recv(fd2, buf, sizeof(buf) - 1, 0);
    ASSERT_GT(n2, 0);
    close(fd2);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(metrics->backend_pool_hits_total.load(), 1u);

    StopProxyLoop();
}

// 14. Backend Connection Pool Reaps Expired Connections On Timeout
TEST_F(L4ProxyIntegrationTest, BackendConnectionPoolReapsExpiredOnTimeout) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_connection_pooling = true;
    config.timeout_check_interval = std::chrono::milliseconds(50);
    config.connection_pool_config.idle_timeout = std::chrono::milliseconds(100);

    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());

    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    sockaddr_in target_addr{};
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(proxy.GetPort());
    inet_pton(AF_INET, "127.0.0.1", &target_addr.sin_addr);

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_EQ(connect(fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    std::string req = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nTest";
    send(fd, req.data(), req.size(), 0);
    char buf[64]{};
    recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);

    // Wait for connection to be recycled into pool
    for (int retry = 0; retry < 50; ++retry) {
        if (proxy.GetBackendPool() && proxy.GetBackendPool()->TotalIdleCount() > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_NE(proxy.GetBackendPool(), nullptr);
    EXPECT_EQ(proxy.GetBackendPool()->TotalIdleCount(), 1u);

    // Wait for idle_timeout (100ms) and periodic CheckTimeouts to reap the expired connection
    for (int retry = 0; retry < 50; ++retry) {
        if (proxy.GetBackendPool()->TotalIdleCount() == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    EXPECT_EQ(proxy.GetBackendPool()->TotalIdleCount(), 0u);

    StopProxyLoop();
}

// 15. L7 Host-Based Routing and Proxy Header Injection
TEST_F(L4ProxyIntegrationTest, HostBasedL7RoutingAndHeaderInjection) {
    TestTcpBackend backend_api;
    ASSERT_TRUE(backend_api.Start());

    TestTcpBackend backend_web;
    ASSERT_TRUE(backend_web.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_l7_routing = true;

    L4ProxyServer proxy(config);
    proxy.AddNamedPool("api_pool", "round_robin");
    proxy.AddBackendToNamedPool("api_pool", "127.0.0.1", backend_api.GetPort());

    proxy.AddNamedPool("web_pool", "round_robin");
    proxy.AddBackendToNamedPool("web_pool", "127.0.0.1", backend_web.GetPort());

    L7RouteConfig r_api;
    r_api.name = "api_route";
    r_api.host = "api.example.com";
    r_api.path_prefix = "/";
    r_api.target_pool = "api_pool";
    proxy.AddRoute(std::move(r_api));

    L7RouteConfig r_web;
    r_web.name = "web_route";
    r_web.host = "web.example.com";
    r_web.path_prefix = "/";
    r_web.target_pool = "web_pool";
    proxy.AddRoute(std::move(r_web));

    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    auto send_request = [&](const std::string& host_header) -> std::string {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GT(fd, 0);
        sockaddr_in target_addr{};
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(proxy.GetPort());
        inet_pton(AF_INET, "127.0.0.1", &target_addr.sin_addr);
        if (connect(fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)) != 0) {
            close(fd);
            return "";
        }
        std::string req = "GET /status HTTP/1.1\r\nHost: " + host_header + "\r\nConnection: close\r\n\r\n";
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

    // Request to api.example.com -> api_pool
    std::string resp_api = send_request("api.example.com");
    EXPECT_NE(resp_api.find("X-Forwarded-Host: api.example.com"), std::string::npos);
    EXPECT_NE(resp_api.find("X-Forwarded-Proto: http"), std::string::npos);
    EXPECT_NE(resp_api.find("X-Forwarded-For: 127.0.0.1"), std::string::npos);
    EXPECT_NE(resp_api.find("X-Request-ID: "), std::string::npos);
    EXPECT_EQ(backend_api.GetAcceptedCount(), 1u);
    EXPECT_EQ(backend_web.GetAcceptedCount(), 0u);

    // Request to web.example.com -> web_pool
    std::string resp_web = send_request("web.example.com");
    EXPECT_NE(resp_web.find("X-Forwarded-Host: web.example.com"), std::string::npos);
    EXPECT_NE(resp_web.find("X-Forwarded-Proto: http"), std::string::npos);
    EXPECT_EQ(backend_api.GetAcceptedCount(), 1u);
    EXPECT_EQ(backend_web.GetAcceptedCount(), 1u);

    StopProxyLoop();
}

// 16. L7 Path-Based Routing with Prefix Stripping
TEST_F(L4ProxyIntegrationTest, PathBasedL7RoutingAndPrefixStripping) {
    TestTcpBackend backend_v1;
    ASSERT_TRUE(backend_v1.Start());

    TestTcpBackend backend_v2;
    ASSERT_TRUE(backend_v2.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_l7_routing = true;

    L4ProxyServer proxy(config);
    proxy.AddNamedPool("v1_pool", "round_robin");
    proxy.AddBackendToNamedPool("v1_pool", "127.0.0.1", backend_v1.GetPort());

    proxy.AddNamedPool("v2_pool", "round_robin");
    proxy.AddBackendToNamedPool("v2_pool", "127.0.0.1", backend_v2.GetPort());

    L7RouteConfig r_v1;
    r_v1.name = "v1_strip";
    r_v1.path_prefix = "/api/v1";
    r_v1.target_pool = "v1_pool";
    r_v1.strip_prefix = true;
    proxy.AddRoute(std::move(r_v1));

    L7RouteConfig r_v2;
    r_v2.name = "v2_keep";
    r_v2.path_prefix = "/api/v2";
    r_v2.target_pool = "v2_pool";
    r_v2.strip_prefix = false;
    proxy.AddRoute(std::move(r_v2));

    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    auto send_path_request = [&](const std::string& path) -> std::string {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GT(fd, 0);
        sockaddr_in target_addr{};
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(proxy.GetPort());
        inet_pton(AF_INET, "127.0.0.1", &target_addr.sin_addr);
        if (connect(fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)) != 0) {
            close(fd);
            return "";
        }
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

    // /api/v1/users should have prefix stripped to /users and route to v1_pool
    std::string resp_v1 = send_path_request("/api/v1/users");
    EXPECT_NE(resp_v1.find("GET /users HTTP/1.1"), std::string::npos);
    EXPECT_EQ(backend_v1.GetAcceptedCount(), 1u);
    EXPECT_EQ(backend_v2.GetAcceptedCount(), 0u);

    // /api/v2/items should keep /api/v2/items and route to v2_pool
    std::string resp_v2 = send_path_request("/api/v2/items");
    EXPECT_NE(resp_v2.find("GET /api/v2/items HTTP/1.1"), std::string::npos);
    EXPECT_EQ(backend_v1.GetAcceptedCount(), 1u);
    EXPECT_EQ(backend_v2.GetAcceptedCount(), 1u);

    StopProxyLoop();
}

// 17. Unmatched Route Returns 404 or Falls Back to Default Pool
TEST_F(L4ProxyIntegrationTest, UnmatchedRouteReturns404OrFallsBackToDefault) {
    TestTcpBackend backend_default;
    ASSERT_TRUE(backend_default.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_l7_routing = true;

    L4ProxyServer proxy(config);
    proxy.AddNamedPool("default_pool", "round_robin");
    proxy.AddBackendToNamedPool("default_pool", "127.0.0.1", backend_default.GetPort());

    L7RouteConfig r_specific;
    r_specific.name = "specific";
    r_specific.path_prefix = "/specific";
    r_specific.target_pool = "default_pool";
    proxy.AddRoute(std::move(r_specific));

    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    auto send_req = [&](const std::string& path) -> std::string {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        EXPECT_GT(fd, 0);
        sockaddr_in target_addr{};
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(proxy.GetPort());
        inet_pton(AF_INET, "127.0.0.1", &target_addr.sin_addr);
        if (connect(fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)) != 0) {
            close(fd);
            return "";
        }
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

    // Request to unmatched route /unknown with no default pool set -> 404 Not Found
    std::string resp_404 = send_req("/unknown");
    EXPECT_NE(resp_404.find("404 Not Found"), std::string::npos);
    EXPECT_EQ(backend_default.GetAcceptedCount(), 0u);

    // Set default pool and retry -> forwards to default pool
    proxy.SetDefaultPool("default_pool");
    std::string resp_fallback = send_req("/unknown");
    EXPECT_NE(resp_fallback.find("GET /unknown HTTP/1.1"), std::string::npos);
    EXPECT_EQ(backend_default.GetAcceptedCount(), 1u);

    StopProxyLoop();
}

// 18. Malformed HTTP Request in L7 Mode Returns 400 Bad Request
TEST_F(L4ProxyIntegrationTest, MalformedRequestReturns400BadRequest) {
    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.enable_l7_routing = true;

    L4ProxyServer proxy(config);
    L7RouteConfig r;
    r.path_prefix = "/";
    r.target_pool = "dummy";
    proxy.AddRoute(std::move(r));

    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(fd, 0);
    sockaddr_in target_addr{};
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(proxy.GetPort());
    inet_pton(AF_INET, "127.0.0.1", &target_addr.sin_addr);
    ASSERT_EQ(connect(fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    std::string bad_req = "NOT_A_VALID_HTTP_REQUEST\r\n\r\n";
    send(fd, bad_req.data(), bad_req.size(), 0);

    std::string resp;
    char buf[512];
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
    }
    close(fd);

    EXPECT_NE(resp.find("400 Bad Request"), std::string::npos);

    StopProxyLoop();
}
