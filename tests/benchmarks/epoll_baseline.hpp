#pragma once

#include "buffer/buffer_pool.hpp"
#include "lb/load_balancer.hpp"
#include "lb/backend.hpp"
#include "lb/round_robin.hpp"

#include <sys/epoll.h>
#include <netinet/in.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <vector>

namespace helios {

// Linux epoll(7) non-blocking L4 proxy baseline.
// Architecturally identical to L4ProxyServer (sharing BufferPool, buffer sizes,
// backend model, and round-robin load balancer), but replacing io_uring with
// standard epoll_create1, epoll_ctl, epoll_wait, and non-blocking sockets.
class EpollBaselineProxy {
public:
    struct Config {
        std::string listen_host{"127.0.0.1"};
        uint16_t listen_port{0}; // 0 = bind ephemeral port
        size_t total_buffers{1024};
        size_t buffer_size{16384};
        int max_events{64};
    };

    EpollBaselineProxy();
    explicit EpollBaselineProxy(Config config);
    ~EpollBaselineProxy();

    // Prevent copy and move to keep file descriptor mappings stable
    EpollBaselineProxy(const EpollBaselineProxy&) = delete;
    EpollBaselineProxy& operator=(const EpollBaselineProxy&) = delete;
    EpollBaselineProxy(EpollBaselineProxy&&) = delete;
    EpollBaselineProxy& operator=(EpollBaselineProxy&&) = delete;

    // Backend & Load Balancer Management
    void AddBackend(std::shared_ptr<Backend> backend);
    void AddBackend(std::string host, uint16_t port, uint32_t weight = 1);
    void SetLoadBalancer(std::shared_ptr<LoadBalancer> lb) noexcept;
    [[nodiscard]] LoadBalancer& GetLoadBalancer() noexcept { return *lb_; }

    // Lifecycle
    bool Start();
    void Stop() noexcept;
    void Run();
    int Step(int timeout_ms = 100);

    // Queries
    [[nodiscard]] bool IsRunning() const noexcept { return running_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint16_t GetPort() const noexcept { return listen_port_; }
    [[nodiscard]] size_t GetActiveConnectionCount() const noexcept;
    [[nodiscard]] uint64_t GetTotalAcceptedConnections() const noexcept { return total_accepted_.load(std::memory_order_relaxed); }
    [[nodiscard]] BufferPool& GetBufferPool() noexcept { return buffer_pool_; }

private:
    struct ConnectionState {
        uint64_t conn_id{0};
        int client_fd{-1};
        int backend_fd{-1};
        std::shared_ptr<Backend> backend;

        bool backend_connecting{true};
        bool client_read_closed{false};
        bool backend_read_closed{false};

        Buffer* c2b_buf{nullptr}; // Data from client pending write to backend
        Buffer* b2c_buf{nullptr}; // Data from backend pending write to client
    };

    void HandleAccept();
    void HandleSocketEvent(int fd, uint32_t events);
    void HandleBackendConnect(ConnectionState& conn);
    void HandleClientRead(ConnectionState& conn);
    void HandleBackendWrite(ConnectionState& conn);
    void HandleBackendRead(ConnectionState& conn);
    void HandleClientWrite(ConnectionState& conn);
    void CloseConnection(uint64_t conn_id);
    void UpdateEpollInterest(int fd, uint32_t events);
    void Cleanup() noexcept;

    Config config_;
    BufferPool buffer_pool_;
    std::shared_ptr<LoadBalancer> lb_;

    std::atomic<int> epoll_fd_{-1};
    std::atomic<int> listen_fd_{-1};
    std::atomic<int> stop_event_fd_{-1};
    uint16_t listen_port_{0};

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> total_accepted_{0};
    uint64_t next_conn_id_{1};

    std::atomic<std::thread::id> runner_thread_id_{};
    mutable std::mutex state_mutex_;

    // Connection tracking
    std::unordered_map<uint64_t, std::unique_ptr<ConnectionState>> connections_;
    std::unordered_map<int, uint64_t> fd_to_conn_id_;
};

} // namespace helios
