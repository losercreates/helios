#pragma once

#include <cstdint>
#include <string>
#include <netinet/in.h>

namespace helios {

class SocketUtils {
public:
    // Create a non-blocking, cloexec TCP listener socket bound to host:port
    static int CreateTcpListener(const char* host, uint16_t port, int backlog = 128, bool reuse_port = false) noexcept;

    // Create a non-blocking, cloexec TCP client socket
    static int CreateTcpSocket() noexcept;

    // Set socket non-blocking mode
    static bool SetNonBlocking(int fd) noexcept;

    // Set SO_REUSEADDR option
    static bool SetReuseAddr(int fd) noexcept;

    // Set SO_REUSEPORT option
    static bool SetReusePort(int fd) noexcept;

    // Disable Nagle's algorithm (TCP_NODELAY)
    static bool SetTcpNoDelay(int fd) noexcept;

    // Populate sockaddr_in structure from host and port
    static bool ParseSockAddr(const char* host, uint16_t port, sockaddr_in* addr) noexcept;

    // Get formatted "IP:Port" string representation of a sockaddr_in
    static std::string FormatEndpoint(const sockaddr_in& addr);

    // Get formatted local endpoint of a socket
    static std::string GetLocalEndpoint(int fd);

    // Get formatted peer endpoint of a socket
    static std::string GetPeerEndpoint(int fd);
};

} // namespace helios
