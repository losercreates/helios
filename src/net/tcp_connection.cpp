#include "tcp_connection.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <cerrno>
#include <utility>

namespace helios {

TcpConnection::TcpConnection(RingEngine& engine) noexcept
    : engine_(engine) {}

TcpConnection::TcpConnection(RingEngine& engine, int existing_fd) noexcept
    : engine_(engine),
      fd_(existing_fd),
      state_(existing_fd >= 0 ? ConnectionState::Connected : ConnectionState::Disconnected) {}

TcpConnection::~TcpConnection() {
    Close();
}

TcpConnection::TcpConnection(TcpConnection&& other) noexcept
    : engine_(other.engine_),
      fd_(other.fd_),
      state_(other.state_) {
    other.fd_ = -1;
    other.state_ = ConnectionState::Disconnected;
}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        state_ = other.state_;
        other.fd_ = -1;
        other.state_ = ConnectionState::Disconnected;
    }
    return *this;
}

bool TcpConnection::AsyncConnect(const char* host, uint16_t port, sockaddr_in* target_addr, OpContext* ctx) {
    if (!ctx || !target_addr) return false;
    if (fd_ < 0) {
        fd_ = SocketUtils::CreateTcpSocket();
        if (fd_ < 0) return false;
    }

    if (!SocketUtils::ParseSockAddr(host, port, target_addr)) {
        return false;
    }

    state_ = ConnectionState::Connecting;
    return engine_.PrepConnect(fd_, reinterpret_cast<const sockaddr*>(target_addr), sizeof(sockaddr_in), ctx);
}

bool TcpConnection::AsyncRead(void* buffer, size_t capacity, OpContext* ctx) {
    if (fd_ < 0 || !ctx || !buffer) return false;
    return engine_.PrepRead(fd_, buffer, capacity, 0, ctx);
}

bool TcpConnection::AsyncWrite(const void* buffer, size_t length, OpContext* ctx) {
    if (fd_ < 0 || !ctx || !buffer) return false;
    return engine_.PrepWrite(fd_, buffer, length, 0, ctx);
}

bool TcpConnection::ShutdownWrite() noexcept {
    if (fd_ < 0) return false;
    int ret = ::shutdown(fd_, SHUT_WR);
    if (ret == 0) {
        state_ = ConnectionState::HalfClosedLocal;
        return true;
    }
    return false;
}

void TcpConnection::Close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        state_ = ConnectionState::Closed;
    }
}

} // namespace helios
