#pragma once

#include "io/ring_engine.hpp"
#include "io/op_context.hpp"
#include "socket_utils.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <memory>
#include <functional>
#include <chrono>

namespace helios {

enum class ConnectionState : uint8_t {
    Init,
    ConnectingBackend,
    Established,
    HalfClosedClient,    // Client sent FIN (SHUT_WR forwarded to backend)
    HalfClosedBackend,   // Backend sent FIN (SHUT_WR forwarded to client)
    PendingCancellation, // Draining / closing, waiting for pending_cqe_count == 0
    Closed
};

std::string_view ToString(ConnectionState state) noexcept;

// Explicit connection lifecycle state machine managing client socket & paired backend socket
class ConnectionPair : public std::enable_shared_from_this<ConnectionPair> {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    ConnectionPair(uint64_t conn_id, RingEngine& engine, int client_fd) noexcept;
    ~ConnectionPair();

    // Prevent copy/move to maintain stable memory address for OpContext tagging
    ConnectionPair(const ConnectionPair&) = delete;
    ConnectionPair& operator=(const ConnectionPair&) = delete;
    ConnectionPair(ConnectionPair&&) = delete;
    ConnectionPair& operator=(ConnectionPair&&) = delete;

    // Queries
    [[nodiscard]] RingEngine& GetEngine() noexcept { return engine_; }
    [[nodiscard]] const RingEngine& GetEngine() const noexcept { return engine_; }
    [[nodiscard]] uint64_t GetId() const noexcept { return conn_id_; }
    [[nodiscard]] int GetClientFd() const noexcept { return client_fd_; }
    [[nodiscard]] int GetBackendFd() const noexcept { return backend_fd_; }
    [[nodiscard]] ConnectionState GetState() const noexcept { return state_; }
    [[nodiscard]] uint32_t GetPendingCqeCount() const noexcept { return pending_cqe_count_; }
    [[nodiscard]] bool IsClosed() const noexcept { return state_ == ConnectionState::Closed; }
    [[nodiscard]] bool IsDraining() const noexcept { return state_ == ConnectionState::PendingCancellation; }
    [[nodiscard]] bool IsClientReadStopped() const noexcept { return client_read_stopped_; }
    [[nodiscard]] bool IsBackendReadStopped() const noexcept { return backend_read_stopped_; }
    [[nodiscard]] TimePoint GetLastActiveTime() const noexcept { return last_active_time_; }

    // State Machine Transitions
    bool TransitionTo(ConnectionState new_state);
    static bool IsValidTransition(ConnectionState from, ConnectionState to) noexcept;

    // Socket Assignment
    void SetBackendFd(int backend_fd) noexcept;

    // In-Flight Operation Counting
    void IncrementPendingCqe() noexcept;
    void DecrementPendingCqe() noexcept;

    // Half-Close & Shutdown Logic
    bool HandleClientFin();
    bool HandleBackendFin();
    void InitiateClose(const char* reason = "Requested");

    // Timeout Checks
    bool CheckHalfCloseTimeout(TimePoint now, std::chrono::milliseconds timeout_limit = std::chrono::milliseconds(5000));
    bool CheckIdleTimeout(TimePoint now, std::chrono::milliseconds idle_limit = std::chrono::milliseconds(30000));

    // Optional Cleanup Callback
    void SetCleanupCallback(std::function<void(uint64_t)> cb) { cleanup_cb_ = std::move(cb); }

    // Update activity timestamp
    void Touch() noexcept { last_active_time_ = Clock::now(); }

private:
    void PerformCleanup() noexcept;

    uint64_t conn_id_{0};
    RingEngine& engine_;
    int client_fd_{-1};
    int backend_fd_{-1};
    ConnectionState state_{ConnectionState::Init};

    uint32_t pending_cqe_count_{0};
    bool client_read_stopped_{false};
    bool backend_read_stopped_{false};

    TimePoint last_active_time_{Clock::now()};
    TimePoint half_close_start_time_{};
    bool half_close_timer_active_{false};

    std::function<void(uint64_t)> cleanup_cb_;
};

} // namespace helios
