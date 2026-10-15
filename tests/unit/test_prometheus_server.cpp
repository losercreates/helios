#include <gtest/gtest.h>
#include "metrics/metrics_collector.hpp"
#include "metrics/prometheus_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace helios;
using namespace std::chrono_literals;

namespace {

int ConnectSocket(uint16_t port, std::chrono::milliseconds timeout = 2000ms) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv{};
    tv.tv_sec = timeout.count() / 1000;
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

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

std::string SendHttpRequest(uint16_t port, const std::string& raw_request) {
    int fd = ConnectSocket(port);
    if (fd < 0) return "";

    size_t total_sent = 0;
    while (total_sent < raw_request.size()) {
        ssize_t n = ::send(fd, raw_request.data() + total_sent, raw_request.size() - total_sent, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            ::close(fd);
            return "";
        }
        total_sent += static_cast<size_t>(n);
    }

    std::string response;
    std::array<char, 2048> buf{};
    while (true) {
        ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n > 0) {
            response.append(buf.data(), static_cast<size_t>(n));
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    ::close(fd);
    return response;
}

} // namespace

// ============================================================================
// 1. Prometheus Text Rendering & Schema Compliance Tests
// ============================================================================

TEST(PrometheusMetricsTest, TextExpositionFormatCompliance) {
    MetricsCollector collector;
    auto w0 = collector.GetOrCreateWorkerMetrics(0);

    w0->requests_total.store(104523);
    w0->bytes_read_total.store(540239042);
    w0->bytes_written_total.store(1240239042);
    w0->active_connections.store(412);

    w0->latency_histogram.Observe(45);
    w0->latency_histogram.Observe(80);
    w0->latency_histogram.Observe(250);
    w0->latency_histogram.Observe(500);

    w0->iouring_sqe_submitted_total.store(209046);
    w0->iouring_cqe_reaped_total.store(209046);
    w0->iouring_sq_overflow_total.store(0);

    w0->buffer_pool_utilization.store(0.35);
    w0->backpressure_pause_events_total.store(4);

    std::string text = collector.RenderPrometheusText();

    // Verify HELP and TYPE comments
    EXPECT_NE(text.find("# HELP helios_requests_total Total number of HTTP requests processed"), std::string::npos);
    EXPECT_NE(text.find("# TYPE helios_requests_total counter"), std::string::npos);
    EXPECT_NE(text.find("helios_requests_total{worker=\"0\", protocol=\"http1\"} 104523"), std::string::npos);

    EXPECT_NE(text.find("# HELP helios_bytes_read_total Total bytes read from client sockets"), std::string::npos);
    EXPECT_NE(text.find("# TYPE helios_bytes_read_total counter"), std::string::npos);
    EXPECT_NE(text.find("helios_bytes_read_total{worker=\"0\"} 540239042"), std::string::npos);

    EXPECT_NE(text.find("# HELP helios_bytes_written_total Total bytes written to client sockets"), std::string::npos);
    EXPECT_NE(text.find("# TYPE helios_bytes_written_total counter"), std::string::npos);
    EXPECT_NE(text.find("helios_bytes_written_total{worker=\"0\"} 1240239042"), std::string::npos);

    EXPECT_NE(text.find("# HELP helios_active_connections Current active client connections"), std::string::npos);
    EXPECT_NE(text.find("# TYPE helios_active_connections gauge"), std::string::npos);
    EXPECT_NE(text.find("helios_active_connections{worker=\"0\"} 412"), std::string::npos);

    // Verify Histogram metrics
    EXPECT_NE(text.find("# HELP helios_added_latency_microseconds Added proxy latency in microseconds"), std::string::npos);
    EXPECT_NE(text.find("# TYPE helios_added_latency_microseconds histogram"), std::string::npos);
    EXPECT_NE(text.find("helios_added_latency_microseconds_bucket{worker=\"0\", le=\"50\"} 1"), std::string::npos);
    EXPECT_NE(text.find("helios_added_latency_microseconds_bucket{worker=\"0\", le=\"100\"} 2"), std::string::npos);
    EXPECT_NE(text.find("helios_added_latency_microseconds_bucket{worker=\"0\", le=\"300\"} 3"), std::string::npos);
    EXPECT_NE(text.find("helios_added_latency_microseconds_bucket{worker=\"0\", le=\"+Inf\"} 4"), std::string::npos);
    EXPECT_NE(text.find("helios_added_latency_microseconds_sum{worker=\"0\"} 875"), std::string::npos);
    EXPECT_NE(text.find("helios_added_latency_microseconds_count{worker=\"0\"} 4"), std::string::npos);

    // Verify io_uring metrics
    EXPECT_NE(text.find("# HELP helios_iouring_sqe_submitted_total Total io_uring SQEs submitted"), std::string::npos);
    EXPECT_NE(text.find("helios_iouring_sqe_submitted_total{worker=\"0\"} 209046"), std::string::npos);
    EXPECT_NE(text.find("helios_iouring_cqe_reaped_total{worker=\"0\"} 209046"), std::string::npos);
    EXPECT_NE(text.find("helios_iouring_sq_overflow_total{worker=\"0\"} 0"), std::string::npos);

    // Verify backpressure & buffer metrics
    EXPECT_NE(text.find("# HELP helios_buffer_pool_utilization Buffer pool utilization ratio (0.0 - 1.0)"), std::string::npos);
    EXPECT_NE(text.find("helios_buffer_pool_utilization{worker=\"0\"} 0.35"), std::string::npos);
    EXPECT_NE(text.find("helios_backpressure_pause_events_total{worker=\"0\"} 4"), std::string::npos);
}

TEST(PrometheusMetricsTest, MultiWorkerIsolationAndSorting) {
    MetricsCollector collector;
    auto w1 = collector.GetOrCreateWorkerMetrics(1);
    auto w0 = collector.GetOrCreateWorkerMetrics(0);

    w0->requests_total.store(100);
    w1->requests_total.store(200);

    std::string text = collector.RenderPrometheusText();

    size_t pos_w0 = text.find("helios_requests_total{worker=\"0\", protocol=\"http1\"} 100");
    size_t pos_w1 = text.find("helios_requests_total{worker=\"1\", protocol=\"http1\"} 200");

    EXPECT_NE(pos_w0, std::string::npos);
    EXPECT_NE(pos_w1, std::string::npos);
    EXPECT_LT(pos_w0, pos_w1); // Sorted by worker ID
}

TEST(PrometheusMetricsTest, ConcurrentMetricUpdates) {
    auto metrics = std::make_shared<WorkerMetrics>(0);
    const int kThreads = 8;
    const int kIterations = 10000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([metrics]() {
            for (int i = 0; i < kIterations; ++i) {
                metrics->requests_total.fetch_add(1, std::memory_order_relaxed);
                metrics->bytes_read_total.fetch_add(100, std::memory_order_relaxed);
                metrics->latency_histogram.Observe(50);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(metrics->requests_total.load(), static_cast<uint64_t>(kThreads * kIterations));
    EXPECT_EQ(metrics->bytes_read_total.load(), static_cast<uint64_t>(kThreads * kIterations * 100));
    EXPECT_EQ(metrics->latency_histogram.GetTotalCount(), static_cast<uint64_t>(kThreads * kIterations));
    EXPECT_EQ(metrics->latency_histogram.GetBucketCount(0), static_cast<uint64_t>(kThreads * kIterations));
}

// ============================================================================
// 2. Live HTTP Telemetry Server Tests
// ============================================================================

TEST(PrometheusServerTest, LifecycleAndPortBinding) {
    auto collector = std::make_shared<MetricsCollector>();
    PrometheusServer::Config config;
    config.port = 0; // Ephemeral port

    PrometheusServer server(collector, config);
    EXPECT_FALSE(server.IsRunning());
    EXPECT_EQ(server.GetPort(), 0);

    ASSERT_TRUE(server.Start());
    EXPECT_TRUE(server.IsRunning());
    EXPECT_GT(server.GetPort(), 0);

    uint16_t assigned_port = server.GetPort();
    EXPECT_EQ(assigned_port, server.GetPort());

    server.Stop();
    EXPECT_FALSE(server.IsRunning());
}

TEST(PrometheusServerTest, GetMetricsEndpoint) {
    auto collector = std::make_shared<MetricsCollector>();
    auto w0 = collector->GetOrCreateWorkerMetrics(0);
    w0->requests_total.store(42);
    w0->bytes_read_total.store(1024);

    PrometheusServer::Config config;
    config.port = 0; // Ephemeral
    PrometheusServer server(collector, config);
    ASSERT_TRUE(server.Start());

    std::string req = "GET /metrics HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string resp = SendHttpRequest(server.GetPort(), req);

    EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(resp.find("Content-Type: text/plain; version=0.0.4; charset=utf-8"), std::string::npos);
    EXPECT_NE(resp.find("helios_requests_total{worker=\"0\", protocol=\"http1\"} 42"), std::string::npos);
    EXPECT_NE(resp.find("helios_bytes_read_total{worker=\"0\"} 1024"), std::string::npos);

    server.Stop();
}

TEST(PrometheusServerTest, GetHealthzEndpoint) {
    auto collector = std::make_shared<MetricsCollector>();
    PrometheusServer::Config config;
    config.port = 0; // Ephemeral
    PrometheusServer server(collector, config);
    ASSERT_TRUE(server.Start());

    std::string req = "GET /healthz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    std::string resp = SendHttpRequest(server.GetPort(), req);

    EXPECT_NE(resp.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(resp.find("\r\n\r\nOK"), std::string::npos);

    server.Stop();
}

TEST(PrometheusServerTest, NotFoundAndMethodNotAllowed) {
    auto collector = std::make_shared<MetricsCollector>();
    PrometheusServer::Config config;
    config.port = 0; // Ephemeral
    PrometheusServer server(collector, config);
    ASSERT_TRUE(server.Start());

    // 1. Unknown route -> 404
    {
        std::string req = "GET /invalid_path HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        std::string resp = SendHttpRequest(server.GetPort(), req);
        EXPECT_NE(resp.find("HTTP/1.1 404 Not Found"), std::string::npos);
    }

    // 2. Unsupported method -> 405
    {
        std::string req = "POST /metrics HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        std::string resp = SendHttpRequest(server.GetPort(), req);
        EXPECT_NE(resp.find("HTTP/1.1 405 Method Not Allowed"), std::string::npos);
    }

    server.Stop();
}
