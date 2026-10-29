#pragma once

#include "io/ring_engine.hpp"
#include "io/op_context.hpp"
#include "buffer/buffer_pool.hpp"
#include "buffer/backpressure_controller.hpp"
#include "net/tcp_listener.hpp"
#include "net/tcp_connection.hpp"
#include "net/connection_pair.hpp"
#include "lb/load_balancer.hpp"
#include "lb/round_robin.hpp"
#include "backend/backend_pool.hpp"
#include "logging/access_logger.hpp"
#include "metrics/metrics_collector.hpp"
#include "routing/l7_router.hpp"
#include "http/http_rewriter.hpp"
#include "http/http_parser.hpp"
#include "security/acl.hpp"
#include "security/rate_limiter.hpp"
#include "security/security_config.hpp"

#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>
#include <cstdint>
#include <chrono>
#include <string>
#include <mutex>

#include "net/connection_slab_pool.hpp"
#include "tls/tls_context.hpp"
#include "tls/tls_stream.hpp"

namespace helios {

enum class ProxyOpRole : uint8_t {
    Accept,
    Connect,
    ClientRead,
    BackendWrite,
    BackendRead,
    ClientWrite,
    Cancel,
    ClientPoll
};

struct ProxyOpContext : public OpContext {
    ProxyOpRole role{ProxyOpRole::Accept};
    std::shared_ptr<ConnectionPair> conn;
    Buffer* buf{nullptr};
};

class L4ProxyServer {
public:
    struct Config {
        std::string listen_host{"127.0.0.1"};
        uint16_t listen_port{8080};
        bool reuse_port{false};
        size_t ring_entries{512};
        size_t total_buffers{1024};
        size_t buffer_size{16384};
        std::chrono::milliseconds half_close_timeout{5000};
        std::chrono::milliseconds idle_timeout{30000};
        std::chrono::milliseconds timeout_check_interval{250};
        bool enable_connection_pooling{false};
        BackendConnectionPoolConfig connection_pool_config{};
        std::string lb_algorithm{"round_robin"};
        bool enable_l7_routing{false};
        std::vector<L7RouteConfig> routes{};
        std::string default_pool{};
        std::chrono::milliseconds header_read_timeout{5000};
        SecurityConfig security{};
        bool enable_lazy_buffer_allocation{true};
        bool enable_tls{false};
        TlsConfig tls_config{};
    };

    struct NamedPool {
        std::string name;
        std::string algorithm{"round_robin"};
        std::unique_ptr<LoadBalancer> lb;
        std::unique_ptr<BackendConnectionPool> pool;
    };

    L4ProxyServer();
    explicit L4ProxyServer(Config config);
    ~L4ProxyServer();

    // Non-copyable, non-movable
    L4ProxyServer(const L4ProxyServer&) = delete;
    L4ProxyServer& operator=(const L4ProxyServer&) = delete;
    L4ProxyServer(L4ProxyServer&&) = delete;
    L4ProxyServer& operator=(L4ProxyServer&&) = delete;

    // Load balancer configuration
    void AddBackend(std::string host, uint16_t port, uint32_t weight = 1);
    void AddBackend(std::shared_ptr<Backend> backend);
    LoadBalancer& GetLoadBalancer() noexcept { return *lb_; }

    // Named pool and L7 route management
    void AddNamedPool(std::string name, std::string algorithm = "round_robin", bool enable_pool = false, BackendConnectionPoolConfig pool_cfg = {});
    void AddBackendToNamedPool(const std::string& pool_name, std::string host, uint16_t port, uint32_t weight = 1);
    void AddBackendToNamedPool(const std::string& pool_name, std::shared_ptr<Backend> backend);
    void AddRoute(L7RouteConfig route);
    void SetDefaultPool(std::string pool_name);
    void ReloadNamedPoolsAndRoutes(const std::vector<BackendPoolConfig>& pools,
                                   const std::vector<L7RouteConfig>& routes,
                                   const std::string& default_pool);
    void ReloadSecurityConfig(const SecurityConfig& security);
    [[nodiscard]] L7Router& GetRouter() noexcept { return router_; }
    [[nodiscard]] const L7Router& GetRouter() const noexcept { return router_; }
    [[nodiscard]] const std::unordered_map<std::string, NamedPool>& GetNamedPools() const noexcept { return named_pools_; }

    // Initialization & Lifecycle
    bool Start();
    void Stop();
    void CloseListener();

    // Single-threaded event loop iteration
    int RunOnce(std::chrono::milliseconds timeout = std::chrono::milliseconds(10));

    // Accessors
    void SetListenPort(uint16_t port) noexcept { config_.listen_port = port; }
    [[nodiscard]] uint16_t GetPort() const noexcept { return listener_.GetPort(); }
    [[nodiscard]] size_t GetActiveConnectionCount() const noexcept {
        return active_connections_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t GetTotalAcceptedConnections() const noexcept {
        return total_accepted_connections_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] TcpListener& GetListener() noexcept { return listener_; }
    [[nodiscard]] BufferPool& GetBufferPool() noexcept { return buffer_pool_; }
    [[nodiscard]] RingEngine& GetEngine() noexcept { return engine_; }
    [[nodiscard]] bool IsRunning() const noexcept { return running_; }

    void SetAccessLogger(std::shared_ptr<AccessLogger> logger) noexcept { access_logger_ = std::move(logger); }
    [[nodiscard]] std::shared_ptr<AccessLogger> GetAccessLogger() const noexcept { return access_logger_; }

    void SetMetrics(std::shared_ptr<WorkerMetrics> metrics) noexcept { metrics_ = std::move(metrics); }
    [[nodiscard]] std::shared_ptr<WorkerMetrics> GetMetrics() const noexcept { return metrics_; }

    void EnableConnectionPooling(BackendConnectionPoolConfig config = {});
    void DisableConnectionPooling() noexcept;
    [[nodiscard]] BackendConnectionPool* GetBackendPool() noexcept { return backend_pool_.get(); }
    [[nodiscard]] const BackendConnectionPool* GetBackendPool() const noexcept { return backend_pool_.get(); }

    void SetAcl(std::shared_ptr<IpAcl> acl) noexcept {
        std::lock_guard lock(security_mutex_);
        acl_ = std::move(acl);
    }
    [[nodiscard]] std::shared_ptr<IpAcl> GetAcl() const noexcept {
        std::lock_guard lock(security_mutex_);
        return acl_;
    }

    void SetRateLimiter(std::shared_ptr<RateLimiter> limiter) noexcept {
        std::lock_guard lock(security_mutex_);
        rate_limiter_ = std::move(limiter);
    }
    [[nodiscard]] std::shared_ptr<RateLimiter> GetRateLimiter() const noexcept {
        std::lock_guard lock(security_mutex_);
        return rate_limiter_;
    }

    void SetTlsContext(std::shared_ptr<TlsContext> tls_context) noexcept {
        std::lock_guard lock(tls_mutex_);
        tls_context_ = std::move(tls_context);
    }
    [[nodiscard]] std::shared_ptr<TlsContext> GetTlsContext() const noexcept {
        std::lock_guard lock(tls_mutex_);
        return tls_context_;
    }

    [[nodiscard]] const BufferPool& GetBufferPool() const noexcept { return buffer_pool_; }

private:
    struct ConnectionStateData;
public:
    [[nodiscard]] const ConnectionSlabPool<ConnectionStateData>& GetSlabPool() const noexcept { return slab_pool_; }

private:

    struct ConnectionStateData {
        std::shared_ptr<ConnectionPair> conn;
        ProxyOpContext client_read_ctx;
        ProxyOpContext client_write_ctx;
        ProxyOpContext backend_read_ctx;
        ProxyOpContext backend_write_ctx;
        ProxyOpContext connect_ctx;
        ProxyOpContext cancel_backend_read_ctx;
        ProxyOpContext client_poll_ctx;

        sockaddr_in backend_addr{};

        bool client_read_active{false};
        bool backend_read_active{false};
        bool client_poll_active{false};

        bool client_read_paused{false};
        bool backend_read_paused{false};

        bool l7_routed{false};
        HttpParser parser{};
        std::string matched_pool;
        std::string raw_request_accumulated;

        std::shared_ptr<Backend> backend;
        std::shared_ptr<AccessLogger> access_logger;
        std::shared_ptr<WorkerMetrics> metrics;

        std::chrono::steady_clock::time_point start_time{std::chrono::steady_clock::now()};
        std::chrono::system_clock::time_point wall_start_time{std::chrono::system_clock::now()};
        std::string client_ip{"127.0.0.1"};
        uint16_t client_port{0};
        std::string upstream_endpoint;
        std::string method{"TCP"};
        std::string uri{"/"};
        std::string http_version{"TCP"};
        int status_code{200};
        uint64_t bytes_sent{0};
        uint64_t bytes_received{0};
        bool parsed_request{false};
        bool parsed_response{false};
        bool is_websocket{false};
        bool websocket_upgraded{false};

        std::unique_ptr<TlsStream> tls_stream;
        bool tls_handshaking{false};

        ~ConnectionStateData() {
            if (backend) {
                backend->DecrementActiveConnections();
            }
            if (tls_stream && metrics) {
                metrics->tls_active_connections.fetch_sub(1, std::memory_order_relaxed);
            }
            if (websocket_upgraded && metrics) {
                metrics->websocket_active_connections.fetch_sub(1, std::memory_order_relaxed);
            }
            auto now = std::chrono::steady_clock::now();
            auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(now - start_time);
            if (metrics) {
                metrics->active_connections.fetch_sub(1, std::memory_order_relaxed);
                metrics->latency_histogram.Observe(static_cast<uint64_t>(duration_us.count()));
            }
            if (access_logger && access_logger->IsRunning()) {
                AccessLogEntry entry;
                entry.timestamp = wall_start_time;
                entry.client_ip = std::move(client_ip);
                entry.client_port = client_port;
                entry.method = std::move(method);
                entry.uri = std::move(uri);
                entry.http_version = std::move(http_version);
                entry.status_code = status_code;
                entry.bytes_sent = bytes_sent;
                entry.bytes_received = bytes_received;
                entry.duration_us = duration_us;
                entry.upstream_endpoint = std::move(upstream_endpoint);
                access_logger->Log(std::move(entry));
            }
        }
    };

    void ScheduleAccept();
    void HandleAcceptCompletion(const CompletionEvent& event);
    void HandleConnectCompletion(ConnectionStateData& data, const CompletionEvent& event);
    void HandleClientReadCompletion(ConnectionStateData& data, const CompletionEvent& event);
    void HandleBackendWriteCompletion(ConnectionStateData& data, const CompletionEvent& event);
    void HandleBackendReadCompletion(ConnectionStateData& data, const CompletionEvent& event);
    void HandleClientWriteCompletion(ConnectionStateData& data, const CompletionEvent& event);
    void HandleClientPollCompletion(ConnectionStateData& data, const CompletionEvent& event);

    void TryStartClientRead(ConnectionStateData& data);
    void TryStartClientPoll(ConnectionStateData& data);
    void TryStartBackendRead(ConnectionStateData& data);
    void SetClientReadPaused(ConnectionStateData& data, bool paused);
    void SetBackendReadPaused(ConnectionStateData& data, bool paused);
    void ResumePausedReads();
    void CheckTimeouts(std::chrono::steady_clock::time_point now);
    void CheckTimeouts();
    void SendDirectClientResponse(ConnectionStateData& data, std::string_view response);

    std::shared_ptr<Backend> SelectBackendFromPool(const std::string& pool_name);
    BackendConnectionPool* GetPoolForName(const std::string& pool_name) noexcept;

    Config config_;
    RingEngine engine_;
    BufferPool buffer_pool_;
    BackpressureController backpressure_ctrl_;
    TcpListener listener_;
    std::unique_ptr<LoadBalancer> lb_;

    uint32_t next_backend_id_{1};
    uint64_t next_conn_id_{1};
    bool running_{false};
    bool accept_in_flight_{false};
    std::atomic<uint64_t> total_accepted_connections_{0};
    std::atomic<size_t> active_connections_{0};

    ProxyOpContext accept_ctx_;
    ProxyOpContext cancel_accept_ctx_;
    sockaddr_in accept_client_addr_{};
    socklen_t accept_client_addr_len_{sizeof(sockaddr_in)};

    std::unordered_map<uint64_t, ConnectionStateData*> connections_;
    ConnectionSlabPool<ConnectionStateData> slab_pool_;
    std::unordered_set<uint64_t> paused_connections_;
    std::chrono::steady_clock::time_point last_timeout_check_{std::chrono::steady_clock::now()};
    std::chrono::milliseconds timeout_check_interval_{250};
    std::unique_ptr<BackendConnectionPool> backend_pool_;
    std::shared_ptr<AccessLogger> access_logger_;
    std::shared_ptr<WorkerMetrics> metrics_;

    L7Router router_;
    std::unordered_map<std::string, NamedPool> named_pools_;
    mutable std::mutex security_mutex_;
    std::shared_ptr<IpAcl> acl_;
    std::shared_ptr<RateLimiter> rate_limiter_;
    mutable std::mutex tls_mutex_;
    std::shared_ptr<TlsContext> tls_context_;
};

} // namespace helios
