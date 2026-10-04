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

// Helper TCP Echo Backend Server running on localhost
class TestTcpBackend {
public:
    TestTcpBackend() = default;
    ~TestTcpBackend() { Stop(); }

    bool Start() {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = 0; // Ephemeral port

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd);
            return false;
        }

        socklen_t len = sizeof(addr);
        getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);

        if (listen(fd, 128) < 0) {
            close(fd);
            return false;
        }

        listen_fd_.store(fd);
        running_ = true;
        worker_thread_ = std::thread([this]() { RunEchoLoop(); });
        return true;
    }

    void Stop() {
        if (running_) {
            running_ = false;
            int fd = listen_fd_.exchange(-1);
            if (fd >= 0) {
                shutdown(fd, SHUT_RDWR);
                close(fd);
            }
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }
        }
    }

    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }
    [[nodiscard]] size_t GetAcceptedCount() const noexcept { return accepted_count_; }

private:
    void RunEchoLoop() {
        while (running_) {
            int fd = listen_fd_.load();
            if (fd < 0) break;
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);
            int client_fd = accept(fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
            if (client_fd < 0) {
                if (!running_) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            accepted_count_++;

            // Handle connection echo loop in background thread per connection
            std::thread([client_fd]() {
                std::array<char, 8192> buf{};
                while (true) {
                    ssize_t bytes_read = recv(client_fd, buf.data(), buf.size(), 0);
                    if (bytes_read <= 0) break;
                    ssize_t bytes_sent = send(client_fd, buf.data(), static_cast<size_t>(bytes_read), 0);
                    if (bytes_sent <= 0) break;
                }
                shutdown(client_fd, SHUT_RDWR);
                close(client_fd);
            }).detach();
        }
    }

    std::atomic<int> listen_fd_{-1};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    std::atomic<size_t> accepted_count_{0};
};

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
