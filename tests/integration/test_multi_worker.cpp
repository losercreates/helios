#include <gtest/gtest.h>
#include "worker.hpp"
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

// 1. WorkerCoreAffinityBinding: Verify worker threads successfully pin to designated CPU cores without system error
TEST(MultiWorkerIntegrationTest, WorkerCoreAffinityBinding) {
    WorkerPoolConfig config;
    config.num_workers = 2;
    config.listen_port = 0;

    WorkerPool pool(config);
    ASSERT_TRUE(pool.Start());
    EXPECT_EQ(pool.GetWorkerCount(), 2u);

    for (size_t i = 0; i < pool.GetWorkerCount(); ++i) {
        const auto& worker = pool.GetWorker(i);
        EXPECT_TRUE(worker.IsRunning());
        EXPECT_TRUE(worker.GetAffinityStatus());
        EXPECT_TRUE(worker.CheckAffinity(worker.GetCoreId()));
    }

    pool.Stop();
    EXPECT_FALSE(pool.IsRunning());
}

// 2. KernelConnectionDistributionSOReusePort: Dispatch concurrent client TCP requests across multiple workers
// and verify that all active workers accept and process connections
TEST(MultiWorkerIntegrationTest, KernelConnectionDistributionSOReusePort) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    WorkerPoolConfig config;
    config.num_workers = 2;
    config.listen_port = 0;

    WorkerPool pool(config);
    pool.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(pool.Start());

    uint16_t proxy_port = pool.GetPort();
    ASSERT_GT(proxy_port, 0);

    // Send 40 requests through proxy
    for (int i = 0; i < 40; ++i) {
        std::string payload = "reuseport-test-msg-" + std::to_string(i);
        std::string resp;
        ASSERT_TRUE(SendEchoRequest(proxy_port, payload, &resp));
        EXPECT_EQ(resp, payload);
    }

    // Assert that total connections were processed
    EXPECT_EQ(pool.GetTotalAcceptedConnections(), 40u);

    // Assert that the kernel distributed connections across both workers via SO_REUSEPORT
    EXPECT_GT(pool.GetWorker(0).GetAcceptedConnections(), 0u);
    EXPECT_GT(pool.GetWorker(1).GetAcceptedConnections(), 0u);
    EXPECT_EQ(pool.GetWorker(0).GetAcceptedConnections() + pool.GetWorker(1).GetAcceptedConnections(), 40u);

    pool.Stop();
}

// 3. MultiWorkerThroughputConcurrency: Validate data integrity and zero connection drops across concurrent client streams
// forwarded by multiple parallel workers
TEST(MultiWorkerIntegrationTest, MultiWorkerThroughputConcurrency) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    WorkerPoolConfig config;
    config.num_workers = 3;
    config.listen_port = 0;

    WorkerPool pool(config);
    pool.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(pool.Start());

    uint16_t proxy_port = pool.GetPort();
    ASSERT_GT(proxy_port, 0);

    const int num_clients = 6;
    const int requests_per_client = 10;
    std::atomic<size_t> successful_requests{0};
    std::vector<std::thread> client_threads;
    client_threads.reserve(num_clients);

    for (int c = 0; c < num_clients; ++c) {
        client_threads.emplace_back([&, c]() {
            for (int r = 0; r < requests_per_client; ++r) {
                std::string msg = "concurrent-client-" + std::to_string(c) + "-req-" + std::to_string(r);
                std::string resp;
                if (SendEchoRequest(proxy_port, msg, &resp)) {
                    if (resp == msg) {
                        successful_requests++;
                    }
                }
            }
        });
    }

    for (auto& t : client_threads) {
        t.join();
    }

    EXPECT_EQ(successful_requests.load(), static_cast<size_t>(num_clients * requests_per_client));
    EXPECT_EQ(pool.GetTotalAcceptedConnections(), static_cast<uint64_t>(num_clients * requests_per_client));

    pool.Stop();
}

// 4. MultiWorkerCoordinatedDrainAndShutdown: Verify coordinated graceful draining on shutdown signal
// without premature resets or leaked sockets/buffers
TEST(MultiWorkerIntegrationTest, MultiWorkerCoordinatedDrainAndShutdown) {
    TestTcpBackend backend;
    ASSERT_TRUE(backend.Start());

    WorkerPoolConfig config;
    config.num_workers = 2;
    config.listen_port = 0;
    config.drain_timeout = 3000ms;

    WorkerPool pool(config);
    pool.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(pool.Start());

    uint16_t proxy_port = pool.GetPort();
    ASSERT_GT(proxy_port, 0);

    // 1. Establish an active long-lived connection before drain signal
    sockaddr_in target_addr{};
    ASSERT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", proxy_port, &target_addr));
    int active_client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(active_client_fd, 0);
    struct timeval tv{};
    tv.tv_sec = 2;
    setsockopt(active_client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ASSERT_EQ(connect(active_client_fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr)), 0);

    // Verify active connection is tracked across workers
    ASSERT_TRUE(WaitFor([&]() { return pool.GetTotalActiveConnections() >= 1u; }));

    // 2. Main thread triggers coordinated graceful shutdown signal via NotifyStop (eventfd)
    pool.NotifyStop();

    // Wait until all workers receive signal and close their listening sockets
    ASSERT_TRUE(WaitFor([&]() { return pool.AreAllListenersClosed(); }));

    // 3. New connection attempts must be rejected because workers closed their listening sockets
    int new_client_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GT(new_client_fd, 0);
    struct timeval reject_tv{};
    reject_tv.tv_usec = 100000; // 100ms
    setsockopt(new_client_fd, SOL_SOCKET, SO_RCVTIMEO, &reject_tv, sizeof(reject_tv));
    int connect_res = connect(new_client_fd, reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr));
    EXPECT_LT(connect_res, 0); // ECONNREFUSED
    ::close(new_client_fd);

    // 4. Critical: Verify established client connection is NOT force-closed during draining!
    std::string payload = "long-lived-active-client-payload";
    ssize_t sent = send(active_client_fd, payload.data(), payload.size(), 0);
    ASSERT_EQ(sent, static_cast<ssize_t>(payload.size()));

    std::array<char, 64> recv_buf{};
    ssize_t recvd = recv(active_client_fd, recv_buf.data(), recv_buf.size(), 0);
    ASSERT_EQ(recvd, static_cast<ssize_t>(payload.size()));
    EXPECT_EQ(std::string(recv_buf.data(), static_cast<size_t>(recvd)), payload);

    // 5. Active client closes cleanly
    ::close(active_client_fd);

    // Wait for all workers to finish draining and join
    pool.Join();
    EXPECT_EQ(pool.GetTotalActiveConnections(), 0u);
}
