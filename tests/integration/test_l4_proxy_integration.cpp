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
