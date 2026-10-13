#include "tcp_listener.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <utility>

namespace helios {

TcpListener::TcpListener(RingEngine& engine) noexcept
    : engine_(engine) {}

TcpListener::~TcpListener() {
    Close();
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : engine_(other.engine_),
      fd_(other.fd_),
      port_(other.port_.load(std::memory_order_relaxed)) {
    other.fd_ = -1;
    other.port_.store(0, std::memory_order_relaxed);
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        port_.store(other.port_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        other.fd_ = -1;
        other.port_.store(0, std::memory_order_relaxed);
    }
    return *this;
}

bool TcpListener::BindAndListen(const char* host, uint16_t port, int backlog, bool reuse_port) {
    Close();
    int ret = SocketUtils::CreateTcpListener(host, port, backlog, reuse_port);
    if (ret < 0) {
        return false;
    }
    fd_ = ret;

    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        port_.store(ntohs(addr.sin_port), std::memory_order_relaxed);
    } else {
        port_.store(port, std::memory_order_relaxed);
    }

    return true;
}

bool TcpListener::AsyncAccept(sockaddr_in* client_addr, socklen_t* addr_len, OpContext* ctx) {
    if (fd_ < 0 || !ctx) return false;
    return engine_.PrepAccept(fd_, reinterpret_cast<sockaddr*>(client_addr), addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC, ctx);
}

void TcpListener::Close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        port_.store(0, std::memory_order_relaxed);
    }
}

} // namespace helios
