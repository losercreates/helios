#include <gtest/gtest.h>
#include "../benchmarks/epoll_baseline.hpp"
#include "../common/test_tcp_backend.hpp"
#include "net/socket_utils.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <future>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <csignal>

using namespace helios;
using namespace std::chrono_literals;

namespace {

static const bool kSigpipeIgnored = []() {
    ::signal(SIGPIPE, SIG_IGN);
    return true;
}();

template <typename Predicate>
bool WaitFor(Predicate&& pred, std::chrono::milliseconds timeout = 3000ms, std::chrono::milliseconds step = 2ms) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < timeout) {
        if (pred()) return true;
        std::this_thread::sleep_for(step);
    }
    return pred();
}

int ConnectClient(uint16_t port, std::chrono::milliseconds timeout = 2000ms) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv{};
    tv.tv_sec = timeout.count() / 1000;
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    if (!SocketUtils::ParseSockAddr("127.0.0.1", port, &addr)) {
        ::close(fd);
        return -1;
    }

    auto start = std::chrono::steady_clock::now();
    int ret = -1;
    while (std::chrono::steady_clock::now() - start < timeout) {
        ret = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
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
        return -1;
    }
    return fd;
}

bool SendAll(int fd, const std::string& data) {
    size_t total = 0;
    while (total < data.size()) {
        ssize_t n = ::send(fd, data.data() + total, data.size() - total, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

bool RecvExact(int fd, size_t expected_bytes, std::string* out) {
    out->clear();
    out->resize(expected_bytes);
    size_t total = 0;
    while (total < expected_bytes) {
        ssize_t n = ::recv(fd, out->data() + total, expected_bytes - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

TEST(EpollBaselineTest, LifecycleAndBinding) {
    EpollBaselineProxy::Config config;
    config.listen_port = 0; // Ephemeral port
    EpollBaselineProxy proxy(config);

    EXPECT_FALSE(proxy.IsRunning());
    EXPECT_EQ(proxy.GetActiveConnectionCount(), 0u);
    EXPECT_EQ(proxy.GetTotalAcceptedConnections(), 0u);

    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());
    proxy.AddBackend("127.0.0.1", backend.GetPort());

    ASSERT_TRUE(proxy.Start());
    EXPECT_TRUE(proxy.IsRunning());
    EXPECT_GT(proxy.GetPort(), 0u);

    std::thread proxy_thread([&proxy]() {
        proxy.Run();
    });

    int client_fd = ConnectClient(proxy.GetPort());
    ASSERT_GE(client_fd, 0);

    EXPECT_TRUE(WaitFor([&proxy]() {
        return proxy.GetActiveConnectionCount() == 1u;
    }));
    EXPECT_EQ(proxy.GetTotalAcceptedConnections(), 1u);

    ::close(client_fd);

    EXPECT_TRUE(WaitFor([&proxy]() {
        return proxy.GetActiveConnectionCount() == 0u;
    }));

    proxy.Stop();
    if (proxy_thread.joinable()) {
        proxy_thread.join();
    }

    EXPECT_FALSE(proxy.IsRunning());
    EXPECT_EQ(proxy.GetBufferPool().CheckedOutCount(), 0u);
}

TEST(EpollBaselineTest, BidirectionalForwarding) {
    EpollBaselineProxy::Config config;
    config.listen_port = 0;
    EpollBaselineProxy proxy(config);

    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());
    proxy.AddBackend("127.0.0.1", backend.GetPort());

    ASSERT_TRUE(proxy.Start());
    std::thread proxy_thread([&proxy]() {
        proxy.Run();
    });

    // 1. Small message round trip
    {
        int client_fd = ConnectClient(proxy.GetPort());
        ASSERT_GE(client_fd, 0);

        std::string req = "Hello, EpollBaselineProxy!";
        ASSERT_TRUE(SendAll(client_fd, req));

        std::string resp;
        ASSERT_TRUE(RecvExact(client_fd, req.size(), &resp));
        EXPECT_EQ(resp, req);

        ::close(client_fd);
    }

    // 2. Large multi-buffer payload round trip (64 KB across four 16 KB buffers)
    {
        int client_fd = ConnectClient(proxy.GetPort());
        ASSERT_GE(client_fd, 0);

        std::string large_req(65536, 'X');
        for (size_t i = 0; i < large_req.size(); ++i) {
            large_req[i] = static_cast<char>('A' + (i % 26));
        }

        std::thread sender([client_fd, &large_req]() {
            EXPECT_TRUE(SendAll(client_fd, large_req));
        });

        std::string large_resp;
        EXPECT_TRUE(RecvExact(client_fd, large_req.size(), &large_resp));
        EXPECT_EQ(large_resp, large_req);

        sender.join();
        ::close(client_fd);
    }

    EXPECT_TRUE(WaitFor([&proxy]() {
        return proxy.GetActiveConnectionCount() == 0u;
    }));

    proxy.Stop();
    if (proxy_thread.joinable()) {
        proxy_thread.join();
    }

    EXPECT_EQ(proxy.GetBufferPool().CheckedOutCount(), 0u);
}

TEST(EpollBaselineTest, MultiConnectionConcurrency) {
    EpollBaselineProxy::Config config;
    config.listen_port = 0;
    config.total_buffers = 2048;
    EpollBaselineProxy proxy(config);

    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());
    proxy.AddBackend("127.0.0.1", backend.GetPort());

    ASSERT_TRUE(proxy.Start());
    std::thread proxy_thread([&proxy]() {
        proxy.Run();
    });

    constexpr size_t kNumClients = 20;
    constexpr size_t kPayloadSize = 8192;
    std::vector<std::thread> client_threads;
    std::atomic<size_t> success_count{0};

    for (size_t i = 0; i < kNumClients; ++i) {
        client_threads.emplace_back([i, port = proxy.GetPort(), &success_count]() {
            int cfd = ConnectClient(port);
            if (cfd < 0) return;

            std::string payload(kPayloadSize, static_cast<char>('0' + (i % 10)));
            if (!SendAll(cfd, payload)) {
                ::close(cfd);
                return;
            }

            std::string resp;
            if (!RecvExact(cfd, payload.size(), &resp)) {
                ::close(cfd);
                return;
            }

            if (resp == payload) {
                ++success_count;
            }
            ::close(cfd);
        });
    }

    for (auto& t : client_threads) {
        if (t.joinable()) t.join();
    }

    EXPECT_EQ(success_count.load(), kNumClients);
    EXPECT_GE(proxy.GetTotalAcceptedConnections(), kNumClients);

    EXPECT_TRUE(WaitFor([&proxy]() {
        return proxy.GetActiveConnectionCount() == 0u;
    }));

    proxy.Stop();
    if (proxy_thread.joinable()) {
        proxy_thread.join();
    }

    EXPECT_EQ(proxy.GetBufferPool().CheckedOutCount(), 0u);
}

TEST(EpollBaselineTest, BackendFailureHandling) {
    // 1. Connection refused handling
    {
        EpollBaselineProxy::Config config;
        config.listen_port = 0;
        EpollBaselineProxy proxy(config);

        // Point backend to an unbound port (e.g., 65530 on 127.0.0.1)
        proxy.AddBackend("127.0.0.1", 65530);
        ASSERT_TRUE(proxy.Start());

        std::thread proxy_thread([&proxy]() {
            proxy.Run();
        });

        int client_fd = ConnectClient(proxy.GetPort());
        ASSERT_GE(client_fd, 0);

        // Send a byte; the proxy should detect backend connection refusal and close client
        SendAll(client_fd, "A");

        char buf[16];
        ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
        // recv should return 0 (EOF) or -1 (ECONNRESET)
        EXPECT_TRUE(n <= 0);

        ::close(client_fd);

        EXPECT_TRUE(WaitFor([&proxy]() {
            return proxy.GetActiveConnectionCount() == 0u;
        }));

        proxy.Stop();
        if (proxy_thread.joinable()) {
            proxy_thread.join();
        }

        EXPECT_EQ(proxy.GetBufferPool().CheckedOutCount(), 0u);
    }

    // 2. Abrupt backend stop during active session
    {
        EpollBaselineProxy::Config config;
        config.listen_port = 0;
        EpollBaselineProxy proxy(config);

        auto backend = std::make_unique<TestTcpBackend>();
        ASSERT_TRUE(backend->Start());
        proxy.AddBackend("127.0.0.1", backend->GetPort());

        ASSERT_TRUE(proxy.Start());
        std::thread proxy_thread([&proxy]() {
            proxy.Run();
        });

        int client_fd = ConnectClient(proxy.GetPort());
        ASSERT_GE(client_fd, 0);

        // Verify working initially
        ASSERT_TRUE(SendAll(client_fd, "Ping"));
        std::string resp;
        ASSERT_TRUE(RecvExact(client_fd, 4, &resp));
        EXPECT_EQ(resp, "Ping");

        // Stop backend abruptly
        backend->Stop();

        // Send further data
        SendAll(client_fd, "AfterStop");

        char buf[16];
        ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
        EXPECT_TRUE(n <= 0);

        ::close(client_fd);

        EXPECT_TRUE(WaitFor([&proxy]() {
            return proxy.GetActiveConnectionCount() == 0u;
        }));

        proxy.Stop();
        if (proxy_thread.joinable()) {
            proxy_thread.join();
        }

        EXPECT_EQ(proxy.GetBufferPool().CheckedOutCount(), 0u);
    }
}

TEST(EpollBaselineTest, SynchronousStepExecution) {
    EpollBaselineProxy::Config config;
    config.listen_port = 0;
    EpollBaselineProxy proxy(config);

    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());
    proxy.AddBackend("127.0.0.1", backend.GetPort());

    ASSERT_TRUE(proxy.Start());

    // Connect client
    int client_fd = ConnectClient(proxy.GetPort());
    ASSERT_GE(client_fd, 0);

    // Drive step synchronously to accept and initiate connect
    EXPECT_GE(proxy.Step(100), 0);

    std::string req = "SyncStepData";
    ASSERT_TRUE(SendAll(client_fd, req));

    // Drive step synchronously to read client and write backend
    int steps = 0;
    while (proxy.Step(10) > 0 && ++steps < 10) {}

    std::string resp;
    ASSERT_TRUE(RecvExact(client_fd, req.size(), &resp));
    EXPECT_EQ(resp, req);

    ::close(client_fd);
    proxy.Step(50);

    proxy.Stop();
    EXPECT_EQ(proxy.GetBufferPool().CheckedOutCount(), 0u);
}
