#include "socket_utils.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

namespace helios {

int SocketUtils::CreateTcpListener(const char* host, uint16_t port, int backlog, bool reuse_port) noexcept {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -errno;
    }

    if (!SetReuseAddr(fd)) {
        int err = errno;
        ::close(fd);
        return -err;
    }

    if (reuse_port && !SetReusePort(fd)) {
        int err = errno;
        ::close(fd);
        return -err;
    }

    sockaddr_in addr{};
    if (!ParseSockAddr(host, port, &addr)) {
        ::close(fd);
        return -EINVAL;
    }

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        int err = errno;
        ::close(fd);
        return -err;
    }

    if (::listen(fd, backlog) < 0) {
        int err = errno;
        ::close(fd);
        return -err;
    }

    return fd;
}

int SocketUtils::CreateTcpSocket() noexcept {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -errno;
    }
    return fd;
}

bool SocketUtils::SetNonBlocking(int fd) noexcept {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

bool SocketUtils::SetReuseAddr(int fd) noexcept {
    int opt = 1;
    return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0;
}

bool SocketUtils::SetReusePort(int fd) noexcept {
    int opt = 1;
    return ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) == 0;
}

bool SocketUtils::SetTcpNoDelay(int fd) noexcept {
    int opt = 1;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == 0;
}

bool SocketUtils::ParseSockAddr(const char* host, uint16_t port, sockaddr_in* addr) noexcept {
    if (!addr) return false;
    std::memset(addr, 0, sizeof(sockaddr_in));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);

    if (!host || std::strlen(host) == 0 || std::strcmp(host, "0.0.0.0") == 0) {
        addr->sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }
    if (std::strcmp(host, "127.0.0.1") == 0 || std::strcmp(host, "localhost") == 0) {
        addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return true;
    }

    return ::inet_pton(AF_INET, host, &addr->sin_addr) == 1;
}

std::string SocketUtils::FormatEndpoint(const sockaddr_in& addr) {
    char ip_str[INET_ADDRSTRLEN]{0};
    if (!::inet_ntop(AF_INET, &addr.sin_addr, ip_str, sizeof(ip_str))) {
        return "unknown:0";
    }
    uint16_t port = ntohs(addr.sin_port);
    return std::string(ip_str) + ":" + std::to_string(port);
}

std::string SocketUtils::GetLocalEndpoint(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return "unknown:0";
    }
    return FormatEndpoint(addr);
}

std::string SocketUtils::GetPeerEndpoint(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return "unknown:0";
    }
    return FormatEndpoint(addr);
}

} // namespace helios
