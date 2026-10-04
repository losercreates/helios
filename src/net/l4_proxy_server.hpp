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

#include <unordered_map>
#include <memory>
#include <vector>
#include <cstdint>
#include <chrono>
#include <string>

namespace helios {

enum class ProxyOpRole : uint8_t {
    Accept,
    Connect,
    ClientRead,
    BackendWrite,
    BackendRead,
    ClientWrite
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
        size_t ring_entries{512};
        size_t total_buffers{1024};
        size_t buffer_size{16384};
        std::chrono::milliseconds half_close_timeout{5000};
        std::chrono::milliseconds idle_timeout{30000};
        std::string lb_algorithm{"round_robin"};
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
    LoadBalancer& GetLoadBalancer() noexcept { return *lb_; }

    // Initialization & Lifecycle
    bool Start();
    void Stop();

    // Single-threaded event loop iteration
    int RunOnce(std::chrono::milliseconds timeout = std::chrono::milliseconds(10));

    // Accessors
    [[nodiscard]] uint16_t GetPort() const noexcept { return listener_.GetPort(); }
    [[nodiscard]] size_t GetActiveConnectionCount() const noexcept { return connections_.size(); }
    [[nodiscard]] BufferPool& GetBufferPool() noexcept { return buffer_pool_; }
    [[nodiscard]] RingEngine& GetEngine() noexcept { return engine_; }
    [[nodiscard]] bool IsRunning() const noexcept { return running_; }

private:

    struct ConnectionStateData {
        std::shared_ptr<ConnectionPair> conn;
        ProxyOpContext client_read_ctx;
        ProxyOpContext client_write_ctx;
        ProxyOpContext backend_read_ctx;
        ProxyOpContext backend_write_ctx;
        ProxyOpContext connect_ctx;

        sockaddr_in backend_addr{};

        bool client_read_active{false};
        bool backend_read_active{false};

        bool client_read_paused{false};
        bool backend_read_paused{false};
    };

    void ScheduleAccept();
    void HandleAcceptCompletion(const CompletionEvent& event);
    void HandleConnectCompletion(ConnectionStateData& data, const CompletionEvent& event);
    void HandleClientReadCompletion(ConnectionStateData& data, const CompletionEvent& event);
    void HandleBackendWriteCompletion(ConnectionStateData& data, const CompletionEvent& event);
    void HandleBackendReadCompletion(ConnectionStateData& data, const CompletionEvent& event);
    void HandleClientWriteCompletion(ConnectionStateData& data, const CompletionEvent& event);

    void TryStartClientRead(ConnectionStateData& data);
    void TryStartBackendRead(ConnectionStateData& data);
    void ResumePausedReads();
    void CheckTimeouts();

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

    ProxyOpContext accept_ctx_;
    sockaddr_in accept_client_addr_{};
    socklen_t accept_client_addr_len_{sizeof(sockaddr_in)};

    std::unordered_map<uint64_t, std::unique_ptr<ConnectionStateData>> connections_;
};

} // namespace helios
