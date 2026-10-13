#include "listener.hpp"
#include "socket_utils.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

namespace helios {

Listener::Listener(ListenerConfig config)
    : config_(std::move(config)) {}

Listener::~Listener() {
    Close();
}

Listener::Listener(Listener&& other) noexcept
    : config_(std::move(other.config_)),
      fd_(other.fd_),
      port_(other.port_.load(std::memory_order_relaxed)) {
    other.fd_ = -1;
    other.port_.store(0, std::memory_order_relaxed);
}

Listener& Listener::operator=(Listener&& other) noexcept {
    if (this != &other) {
        Close();
        config_ = std::move(other.config_);
        fd_ = other.fd_;
        port_.store(other.port_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        other.fd_ = -1;
        other.port_.store(0, std::memory_order_relaxed);
    }
    return *this;
}

bool Listener::BindAndListen() {
    return BindAndListen(config_);
}

bool Listener::BindAndListen(const ListenerConfig& config) {
    Close();
    config_ = config;

    int ret = SocketUtils::CreateTcpListener(
        config_.host.c_str(),
        config_.port,
        config_.backlog,
        config_.reuse_port
    );

    if (ret < 0) {
        return false;
    }

    fd_ = ret;

    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        port_.store(ntohs(addr.sin_port), std::memory_order_relaxed);
    } else {
        port_.store(config_.port, std::memory_order_relaxed);
    }

    return true;
}

void Listener::Close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        port_.store(0, std::memory_order_relaxed);
    }
}

} // namespace helios
