#include "net/l4_proxy_server.hpp"
#include "epoll_baseline.hpp"
#include "common/test_tcp_backend.hpp"

#include <benchmark/benchmark.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <csignal>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <stdexcept>
#include <atomic>

using namespace helios;

namespace {

// Ensure SIGPIPE does not abort benchmark process on unexpected peer disconnects
struct SignalIgnorer {
    SignalIgnorer() {
        ::signal(SIGPIPE, SIG_IGN);
    }
} g_signal_ignorer;

// Connect non-blocking client socket with timeout and TCP_NODELAY
int ConnectClient(uint16_t port, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;

    int opt = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Expand socket buffers to ensure high throughput
    int buf_size = 212992;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    addr.sin_port = htons(port);

    int ret = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && (errno == EINPROGRESS || errno == EWOULDBLOCK)) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int timeout_ms = static_cast<int>(timeout.count());
        ret = ::poll(&pfd, 1, timeout_ms);
        if (ret > 0 && (pfd.revents & POLLOUT)) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                return fd;
            }
        }
        ::close(fd);
        return -1;
    } else if (ret == 0) {
        return fd;
    }

    ::close(fd);
    return -1;
}

// Deadlock-free non-blocking bidirectional send and receive loop
bool SendRecvLoop(int fd, const char* send_buf, size_t send_len, char* recv_buf, size_t recv_len) {
    size_t sent = 0;
    size_t recvd = 0;
    pollfd pfd{};
    pfd.fd = fd;

    while (sent < send_len || recvd < recv_len) {
        short events = 0;
        if (recvd < recv_len) events |= POLLIN;
        if (sent < send_len) events |= POLLOUT;
        pfd.events = events;
        pfd.revents = 0;

        int ret = ::poll(&pfd, 1, 5000);
        if (ret <= 0) {
            if (ret < 0 && errno == EINTR) continue;
            return false;
        }

        if ((pfd.revents & POLLIN) && recvd < recv_len) {
            ssize_t n = ::recv(fd, recv_buf + recvd, recv_len - recvd, 0);
            if (n > 0) {
                recvd += static_cast<size_t>(n);
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                return false;
            } else if (n == 0) {
                return false;
            }
        }

        if ((pfd.revents & POLLOUT) && sent < send_len) {
            ssize_t n = ::send(fd, send_buf + sent, send_len - sent, MSG_NOSIGNAL);
            if (n > 0) {
                sent += static_cast<size_t>(n);
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                return false;
            }
        }
    }
    return true;
}

// Scoped runner for Helios io_uring L4ProxyServer
class IoUringProxyRunner {
public:
    explicit IoUringProxyRunner(uint16_t backend_port) {
        L4ProxyServer::Config config;
        config.listen_host = "127.0.0.1";
        config.listen_port = 0; // Ephemeral
        config.total_buffers = 1024;
        config.buffer_size = 16384;
        proxy_ = std::make_unique<L4ProxyServer>(config);
        proxy_->AddBackend("127.0.0.1", backend_port);
        if (!proxy_->Start()) {
            throw std::runtime_error("Failed to start L4ProxyServer");
        }
        port_ = proxy_->GetPort();
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this]() {
            while (running_.load(std::memory_order_relaxed)) {
                int n = proxy_->RunOnce(std::chrono::milliseconds(1));
                if (n == 0) {
                    std::this_thread::yield();
                }
            }
            proxy_->Stop();
        });
    }

    ~IoUringProxyRunner() {
        Stop();
    }

    void Stop() {
        if (running_.exchange(false, std::memory_order_acq_rel)) {
            if (thread_.joinable()) {
                thread_.join();
            }
        }
    }

    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }
    [[nodiscard]] L4ProxyServer& GetProxy() noexcept { return *proxy_; }

private:
    std::unique_ptr<L4ProxyServer> proxy_;
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// Scoped runner for Linux epoll(7) EpollBaselineProxy
class EpollProxyRunner {
public:
    explicit EpollProxyRunner(uint16_t backend_port) {
        EpollBaselineProxy::Config config;
        config.listen_host = "127.0.0.1";
        config.listen_port = 0; // Ephemeral
        config.total_buffers = 1024;
        config.buffer_size = 16384;
        proxy_ = std::make_unique<EpollBaselineProxy>(config);
        proxy_->AddBackend("127.0.0.1", backend_port);
        if (!proxy_->Start()) {
            throw std::runtime_error("Failed to start EpollBaselineProxy");
        }
        port_ = proxy_->GetPort();
        thread_ = std::thread([this]() {
            proxy_->Run();
        });
    }

    ~EpollProxyRunner() {
        Stop();
    }

    void Stop() {
        if (proxy_ && proxy_->IsRunning()) {
            proxy_->Stop();
            if (thread_.joinable()) {
                thread_.join();
            }
        }
    }

    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }
    [[nodiscard]] EpollBaselineProxy& GetProxy() noexcept { return *proxy_; }

private:
    std::unique_ptr<EpollBaselineProxy> proxy_;
    uint16_t port_{0};
    std::thread thread_;
};

} // namespace

// ============================================================================
// 1. Throughput & Payload Scaling Benchmarks (io_uring vs epoll)
// ============================================================================

static void BM_Proxy_IoUring_Throughput(benchmark::State& state) {
    const size_t payload_size = static_cast<size_t>(state.range(0));

    TestTcpBackend backend;
    if (!backend.Start()) {
        state.SkipWithError("Failed to start TestTcpBackend");
        return;
    }

    IoUringProxyRunner runner(backend.GetPort());
    int client_fd = ConnectClient(runner.GetPort());
    if (client_fd < 0) {
        state.SkipWithError("Failed to connect client to L4ProxyServer");
        return;
    }

    // Warm-up ping-pong handshake
    std::string warmup_ping = "PING";
    std::string warmup_pong(4, '\0');
    if (!SendRecvLoop(client_fd, warmup_ping.data(), warmup_ping.size(), warmup_pong.data(), warmup_pong.size())) {
        ::close(client_fd);
        state.SkipWithError("Warmup handshake failed");
        return;
    }

    std::string req(payload_size, 'A');
    std::string resp(payload_size, '\0');

    for (auto _ : state) {
        if (!SendRecvLoop(client_fd, req.data(), req.size(), resp.data(), resp.size())) {
            state.SkipWithError("Data transfer failed");
            break;
        }
    }

    ::close(client_fd);
    runner.Stop();
    backend.Stop();

    int64_t total_ops = static_cast<int64_t>(state.iterations());
    int64_t total_bytes = total_ops * static_cast<int64_t>(payload_size);
    state.SetBytesProcessed(total_bytes);
    state.counters["ops/sec"] = benchmark::Counter(static_cast<double>(total_ops), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_Proxy_IoUring_Throughput)->RangeMultiplier(4)->Range(1024, 65536);

static void BM_Proxy_Epoll_Throughput(benchmark::State& state) {
    const size_t payload_size = static_cast<size_t>(state.range(0));

    TestTcpBackend backend;
    if (!backend.Start()) {
        state.SkipWithError("Failed to start TestTcpBackend");
        return;
    }

    EpollProxyRunner runner(backend.GetPort());
    int client_fd = ConnectClient(runner.GetPort());
    if (client_fd < 0) {
        state.SkipWithError("Failed to connect client to EpollBaselineProxy");
        return;
    }

    // Warm-up ping-pong handshake
    std::string warmup_ping = "PING";
    std::string warmup_pong(4, '\0');
    if (!SendRecvLoop(client_fd, warmup_ping.data(), warmup_ping.size(), warmup_pong.data(), warmup_pong.size())) {
        ::close(client_fd);
        state.SkipWithError("Warmup handshake failed");
        return;
    }

    std::string req(payload_size, 'A');
    std::string resp(payload_size, '\0');

    for (auto _ : state) {
        if (!SendRecvLoop(client_fd, req.data(), req.size(), resp.data(), resp.size())) {
            state.SkipWithError("Data transfer failed");
            break;
        }
    }

    ::close(client_fd);
    runner.Stop();
    backend.Stop();

    int64_t total_ops = static_cast<int64_t>(state.iterations());
    int64_t total_bytes = total_ops * static_cast<int64_t>(payload_size);
    state.SetBytesProcessed(total_bytes);
    state.counters["ops/sec"] = benchmark::Counter(static_cast<double>(total_ops), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_Proxy_Epoll_Throughput)->RangeMultiplier(4)->Range(1024, 65536);

// ============================================================================
// 2. Concurrency & Connection Scaling Benchmarks (io_uring vs epoll)
// ============================================================================

static void BM_Proxy_IoUring_Concurrency(benchmark::State& state) {
    const size_t concurrency = static_cast<size_t>(state.range(0));
    const size_t payload_size = 1024; // 1 KB per concurrent ping-pong

    TestTcpBackend backend;
    if (!backend.Start()) {
        state.SkipWithError("Failed to start TestTcpBackend");
        return;
    }

    IoUringProxyRunner runner(backend.GetPort());
    std::vector<int> client_fds;
    client_fds.reserve(concurrency);

    for (size_t i = 0; i < concurrency; ++i) {
        int fd = ConnectClient(runner.GetPort());
        if (fd < 0) {
            for (int cfd : client_fds) ::close(cfd);
            state.SkipWithError("Failed to connect concurrent client");
            return;
        }
        client_fds.push_back(fd);
    }

    // Warm-up all connections
    std::string warmup_ping = "PING";
    std::string warmup_pong(4, '\0');
    for (int fd : client_fds) {
        SendRecvLoop(fd, warmup_ping.data(), warmup_ping.size(), warmup_pong.data(), warmup_pong.size());
    }

    std::string req(payload_size, 'B');
    std::vector<char> recv_buffer(payload_size * concurrency, '\0');
    std::vector<pollfd> pfds(concurrency);

    for (auto _ : state) {
        // Issue concurrent requests across all client connections
        for (size_t i = 0; i < concurrency; ++i) {
            ssize_t n = ::send(client_fds[i], req.data(), req.size(), MSG_NOSIGNAL);
            (void)n;
            pfds[i].fd = client_fds[i];
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }

        // Reap responses across all concurrent connections
        std::vector<size_t> bytes_recvd(concurrency, 0);
        size_t completed = 0;

        while (completed < concurrency) {
            int ret = ::poll(pfds.data(), static_cast<nfds_t>(concurrency), 5000);
            if (ret <= 0) {
                if (ret < 0 && errno == EINTR) continue;
                state.SkipWithError("Poll timeout during concurrent reap");
                break;
            }

            for (size_t i = 0; i < concurrency; ++i) {
                if (bytes_recvd[i] < payload_size && (pfds[i].revents & POLLIN)) {
                    ssize_t n = ::recv(client_fds[i], recv_buffer.data() + (i * payload_size) + bytes_recvd[i],
                                       payload_size - bytes_recvd[i], 0);
                    if (n > 0) {
                        bytes_recvd[i] += static_cast<size_t>(n);
                        if (bytes_recvd[i] == payload_size) {
                            completed++;
                            pfds[i].events = 0; // Stop polling this socket
                        }
                    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        completed++;
                    }
                }
            }
        }
    }

    for (int fd : client_fds) {
        ::close(fd);
    }
    runner.Stop();
    backend.Stop();

    int64_t total_ops = static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(concurrency);
    int64_t total_bytes = total_ops * static_cast<int64_t>(payload_size);
    state.SetBytesProcessed(total_bytes);
    state.counters["ops/sec"] = benchmark::Counter(static_cast<double>(total_ops), benchmark::Counter::kIsRate);
    state.counters["concurrency"] = benchmark::Counter(static_cast<double>(concurrency));
}
BENCHMARK(BM_Proxy_IoUring_Concurrency)->Arg(1)->Arg(4)->Arg(16)->Arg(64);

static void BM_Proxy_Epoll_Concurrency(benchmark::State& state) {
    const size_t concurrency = static_cast<size_t>(state.range(0));
    const size_t payload_size = 1024; // 1 KB per concurrent ping-pong

    TestTcpBackend backend;
    if (!backend.Start()) {
        state.SkipWithError("Failed to start TestTcpBackend");
        return;
    }

    EpollProxyRunner runner(backend.GetPort());
    std::vector<int> client_fds;
    client_fds.reserve(concurrency);

    for (size_t i = 0; i < concurrency; ++i) {
        int fd = ConnectClient(runner.GetPort());
        if (fd < 0) {
            for (int cfd : client_fds) ::close(cfd);
            state.SkipWithError("Failed to connect concurrent client");
            return;
        }
        client_fds.push_back(fd);
    }

    // Warm-up all connections
    std::string warmup_ping = "PING";
    std::string warmup_pong(4, '\0');
    for (int fd : client_fds) {
        SendRecvLoop(fd, warmup_ping.data(), warmup_ping.size(), warmup_pong.data(), warmup_pong.size());
    }

    std::string req(payload_size, 'B');
    std::vector<char> recv_buffer(payload_size * concurrency, '\0');
    std::vector<pollfd> pfds(concurrency);

    for (auto _ : state) {
        // Issue concurrent requests across all client connections
        for (size_t i = 0; i < concurrency; ++i) {
            ssize_t n = ::send(client_fds[i], req.data(), req.size(), MSG_NOSIGNAL);
            (void)n;
            pfds[i].fd = client_fds[i];
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }

        // Reap responses across all concurrent connections
        std::vector<size_t> bytes_recvd(concurrency, 0);
        size_t completed = 0;

        while (completed < concurrency) {
            int ret = ::poll(pfds.data(), static_cast<nfds_t>(concurrency), 5000);
            if (ret <= 0) {
                if (ret < 0 && errno == EINTR) continue;
                state.SkipWithError("Poll timeout during concurrent reap");
                break;
            }

            for (size_t i = 0; i < concurrency; ++i) {
                if (bytes_recvd[i] < payload_size && (pfds[i].revents & POLLIN)) {
                    ssize_t n = ::recv(client_fds[i], recv_buffer.data() + (i * payload_size) + bytes_recvd[i],
                                       payload_size - bytes_recvd[i], 0);
                    if (n > 0) {
                        bytes_recvd[i] += static_cast<size_t>(n);
                        if (bytes_recvd[i] == payload_size) {
                            completed++;
                            pfds[i].events = 0; // Stop polling this socket
                        }
                    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        completed++;
                    }
                }
            }
        }
    }

    for (int fd : client_fds) {
        ::close(fd);
    }
    runner.Stop();
    backend.Stop();

    int64_t total_ops = static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(concurrency);
    int64_t total_bytes = total_ops * static_cast<int64_t>(payload_size);
    state.SetBytesProcessed(total_bytes);
    state.counters["ops/sec"] = benchmark::Counter(static_cast<double>(total_ops), benchmark::Counter::kIsRate);
    state.counters["concurrency"] = benchmark::Counter(static_cast<double>(concurrency));
}
BENCHMARK(BM_Proxy_Epoll_Concurrency)->Arg(1)->Arg(4)->Arg(16)->Arg(64);

// ============================================================================
// 3. Round-Trip Latency Ping-Pong (io_uring vs epoll)
// ============================================================================

static void BM_Proxy_IoUring_RoundTripPingPong(benchmark::State& state) {
    TestTcpBackend backend;
    if (!backend.Start()) {
        state.SkipWithError("Failed to start TestTcpBackend");
        return;
    }

    IoUringProxyRunner runner(backend.GetPort());
    int client_fd = ConnectClient(runner.GetPort());
    if (client_fd < 0) {
        state.SkipWithError("Failed to connect client");
        return;
    }

    std::string ping(128, 'P');
    std::string pong(128, '\0');

    for (auto _ : state) {
        if (!SendRecvLoop(client_fd, ping.data(), ping.size(), pong.data(), pong.size())) {
            state.SkipWithError("Ping-pong transfer failed");
            break;
        }
    }

    ::close(client_fd);
    runner.Stop();
    backend.Stop();

    state.counters["ops/sec"] = benchmark::Counter(static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_Proxy_IoUring_RoundTripPingPong);

static void BM_Proxy_Epoll_RoundTripPingPong(benchmark::State& state) {
    TestTcpBackend backend;
    if (!backend.Start()) {
        state.SkipWithError("Failed to start TestTcpBackend");
        return;
    }

    EpollProxyRunner runner(backend.GetPort());
    int client_fd = ConnectClient(runner.GetPort());
    if (client_fd < 0) {
        state.SkipWithError("Failed to connect client");
        return;
    }

    std::string ping(128, 'P');
    std::string pong(128, '\0');

    for (auto _ : state) {
        if (!SendRecvLoop(client_fd, ping.data(), ping.size(), pong.data(), pong.size())) {
            state.SkipWithError("Ping-pong transfer failed");
            break;
        }
    }

    ::close(client_fd);
    runner.Stop();
    backend.Stop();

    state.counters["ops/sec"] = benchmark::Counter(static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_Proxy_Epoll_RoundTripPingPong);
