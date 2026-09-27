#pragma once

#include "io/ring_engine.hpp"
#include "io/op_context.hpp"
#include "socket_utils.hpp"
#include <netinet/in.h>
#include <string>

namespace helios {

// Thin C++20 abstraction for a non-blocking TCP listening socket using io_uring async accept
class TcpListener {
public:
    explicit TcpListener(RingEngine& engine) noexcept;
    ~TcpListener();

    // Prevent copying
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // Move semantics
    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;

    // Bind and listen to host:port
    bool BindAndListen(const char* host, uint16_t port, int backlog = 128, bool reuse_port = false);

    // Prepare async accept operation via io_uring
    bool AsyncAccept(sockaddr_in* client_addr, socklen_t* addr_len, OpContext* ctx);

    // Close listener socket
    void Close() noexcept;

    [[nodiscard]] int GetFd() const noexcept { return fd_; }
    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }
    [[nodiscard]] std::string GetEndpoint() const { return SocketUtils::GetLocalEndpoint(fd_); }
    [[nodiscard]] bool IsListening() const noexcept { return fd_ >= 0; }

private:
    RingEngine& engine_;
    int fd_{-1};
    uint16_t port_{0};
};

} // namespace helios
