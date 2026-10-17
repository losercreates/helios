#include <gtest/gtest.h>
#include "worker.hpp"
#include "net/socket_utils.hpp"
#include "config/config_loader.hpp"
#include "../common/test_tcp_backend.hpp"

#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <atomic>
#include <memory>
#include <fstream>
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

// 5. MultiWorkerDynamicBackendHotReload: Verify dynamic zero-downtime hot reloading of backends
TEST(MultiWorkerIntegrationTest, MultiWorkerDynamicBackendHotReload) {
    TestTcpBackend backend1;
    ASSERT_TRUE(backend1.Start());

    TestTcpBackend backend2;
    ASSERT_TRUE(backend2.Start());

    WorkerPoolConfig config;
    config.num_workers = 2;
    config.listen_port = 0;
    config.lb_algorithm = "round_robin";

    WorkerPool pool(config);
    // Start with ONLY backend1
    pool.AddBackend("127.0.0.1", backend1.GetPort(), 1);
    ASSERT_TRUE(pool.Start());

    uint16_t proxy_port = pool.GetPort();
    ASSERT_GT(proxy_port, 0);

    // Send 10 requests: 100% must go to backend1
    for (int i = 0; i < 10; ++i) {
        std::string resp;
        ASSERT_TRUE(SendEchoRequest(proxy_port, "req1", &resp));
        EXPECT_EQ(resp, "req1");
    }
    EXPECT_EQ(backend1.GetAcceptedCount(), 10u);
    EXPECT_EQ(backend2.GetAcceptedCount(), 0u);

    // Dynamically hot-reload backends: now include backend1 AND backend2
    pool.ReloadBackends({
        {"127.0.0.1", backend1.GetPort(), 1},
        {"127.0.0.1", backend2.GetPort(), 1}
    });

    // Send requests: traffic should now reach backend2 as well
    backend1.ResetAcceptedCount();
    backend2.ResetAcceptedCount();

    for (int i = 0; i < 20; ++i) {
        std::string resp;
        ASSERT_TRUE(SendEchoRequest(proxy_port, "req2", &resp));
        EXPECT_EQ(resp, "req2");
    }
    EXPECT_GT(backend1.GetAcceptedCount(), 0u);
    EXPECT_GT(backend2.GetAcceptedCount(), 0u);
    EXPECT_EQ(backend1.GetAcceptedCount() + backend2.GetAcceptedCount(), 20u);

    // Dynamically hot-reload backends: REMOVE backend1, keep ONLY backend2
    pool.ReloadBackends({
        {"127.0.0.1", backend2.GetPort(), 1}
    });

    backend1.ResetAcceptedCount();
    backend2.ResetAcceptedCount();

    for (int i = 0; i < 10; ++i) {
        std::string resp;
        ASSERT_TRUE(SendEchoRequest(proxy_port, "req3", &resp));
        EXPECT_EQ(resp, "req3");
    }
    EXPECT_EQ(backend1.GetAcceptedCount(), 0u);
    EXPECT_EQ(backend2.GetAcceptedCount(), 10u);

    pool.Stop();
}

// 6. SighupHotReloadEndToEnd: Verify end-to-end configuration reload from YAML file
TEST(MultiWorkerIntegrationTest, SighupHotReloadEndToEnd) {
    TestTcpBackend backend1;
    ASSERT_TRUE(backend1.Start());

    TestTcpBackend backend2;
    ASSERT_TRUE(backend2.Start());

    std::string tmp_yaml = "/tmp/helios_sighup_test_" + std::to_string(getpid()) + ".yaml";
    auto write_config = [&](uint16_t b1_port, uint16_t b2_port, bool include_b2) {
        std::ofstream ofs(tmp_yaml);
        ofs << "server:\n";
        ofs << "  host: \"127.0.0.1\"\n";
        ofs << "  port: 0\n";
        ofs << "  workers: 2\n";
        ofs << "backend_pools:\n";
        ofs << "  - name: \"test_pool\"\n";
        ofs << "    algorithm: \"round_robin\"\n";
        ofs << "    endpoints:\n";
        ofs << "      - host: \"127.0.0.1\"\n";
        ofs << "        port: " << b1_port << "\n";
        ofs << "        weight: 1\n";
        if (include_b2) {
            ofs << "      - host: \"127.0.0.1\"\n";
            ofs << "        port: " << b2_port << "\n";
            ofs << "        weight: 1\n";
        }
        ofs.close();
    };

    write_config(backend1.GetPort(), backend2.GetPort(), false);

    auto load_res = ConfigLoader::LoadFromFile(tmp_yaml);
    ASSERT_TRUE(load_res.success);

    WorkerPool pool(load_res.config.ToWorkerPoolConfig());
    for (const auto& pool_cfg : load_res.config.backend_pools) {
        for (const auto& ep : pool_cfg.endpoints) {
            pool.AddBackend(ep.host, ep.port, ep.weight);
        }
    }
    ASSERT_TRUE(pool.Start());
    uint16_t proxy_port = pool.GetPort();

    // Verify initial routing: only backend1 receives traffic
    for (int i = 0; i < 5; ++i) {
        std::string resp;
        ASSERT_TRUE(SendEchoRequest(proxy_port, "initial", &resp));
        EXPECT_EQ(resp, "initial");
    }
    EXPECT_EQ(backend1.GetAcceptedCount(), 5u);
    EXPECT_EQ(backend2.GetAcceptedCount(), 0u);

    // Update the YAML configuration on disk to include backend2
    write_config(backend1.GetPort(), backend2.GetPort(), true);

    // Re-load configuration from file (routine executed on SIGHUP)
    auto reload_res = ConfigLoader::LoadFromFile(tmp_yaml);
    ASSERT_TRUE(reload_res.success);

    std::vector<WorkerPool::BackendInfo> new_endpoints;
    for (const auto& pool_cfg : reload_res.config.backend_pools) {
        for (const auto& ep : pool_cfg.endpoints) {
            new_endpoints.push_back({ep.host, ep.port, ep.weight});
        }
    }
    pool.ReloadBackends(new_endpoints);

    backend1.ResetAcceptedCount();
    backend2.ResetAcceptedCount();

    // Send requests: verify traffic now routes to backend2 as well
    for (int i = 0; i < 10; ++i) {
        std::string resp;
        ASSERT_TRUE(SendEchoRequest(proxy_port, "reloaded", &resp));
        EXPECT_EQ(resp, "reloaded");
    }
    EXPECT_GT(backend2.GetAcceptedCount(), 0u);

    pool.Stop();
    std::remove(tmp_yaml.c_str());
}
