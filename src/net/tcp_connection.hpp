#pragma once

#include "io/ring_engine.hpp"
#include "io/op_context.hpp"
#include "socket_utils.hpp"
#include <netinet/in.h>
#include <string>

namespace helios {

enum class ConnectionState : uint8_t {
    Disconnected,
    Connecting,
    Connected,
    HalfClosedLocal,
    HalfClosedPeer,
    Closed
};

// Thin C++20 abstraction for a non-blocking TCP socket connection using io_uring async I/O
class TcpConnection {
public:
    explicit TcpConnection(RingEngine& engine) noexcept;
    TcpConnection(RingEngine& engine, int existing_fd) noexcept;
    ~TcpConnection();

    // Non-copyable
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    // Move semantics
    TcpConnection(TcpConnection&& other) noexcept;
    TcpConnection& operator=(TcpConnection&& other) noexcept;

    // Initiate async connect to target host:port
    bool AsyncConnect(const char* host, uint16_t port, sockaddr_in* target_addr, OpContext* ctx);

    // Prepare async read via io_uring
    bool AsyncRead(void* buffer, size_t capacity, OpContext* ctx);

    // Prepare async write via io_uring
    bool AsyncWrite(const void* buffer, size_t length, OpContext* ctx);

    // Orderly half-close (SHUT_WR)
    bool ShutdownWrite() noexcept;

    // Close underlying socket descriptor
    void Close() noexcept;

    [[nodiscard]] int GetFd() const noexcept { return fd_; }
    [[nodiscard]] ConnectionState GetState() const noexcept { return state_; }
    [[nodiscard]] bool IsOpen() const noexcept { return fd_ >= 0; }
    [[nodiscard]] std::string GetLocalEndpoint() const { return SocketUtils::GetLocalEndpoint(fd_); }
    [[nodiscard]] std::string GetPeerEndpoint() const { return SocketUtils::GetPeerEndpoint(fd_); }

    void SetState(ConnectionState state) noexcept { state_ = state; }

private:
    RingEngine& engine_;
    int fd_{-1};
    ConnectionState state_{ConnectionState::Disconnected};
};

} // namespace helios
