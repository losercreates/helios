#include "connection_pair.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <iostream>

namespace helios {

std::string_view ToString(ConnectionState state) noexcept {
    switch (state) {
        case ConnectionState::Init: return "Init";
        case ConnectionState::ConnectingBackend: return "ConnectingBackend";
        case ConnectionState::Established: return "Established";
        case ConnectionState::HalfClosedClient: return "HalfClosedClient";
        case ConnectionState::HalfClosedBackend: return "HalfClosedBackend";
        case ConnectionState::PendingCancellation: return "PendingCancellation";
        case ConnectionState::Closed: return "Closed";
    }
    return "Unknown";
}

ConnectionPair::ConnectionPair(uint64_t conn_id, RingEngine& engine, int client_fd) noexcept
    : conn_id_(conn_id),
      engine_(engine),
      client_fd_(client_fd) {}

ConnectionPair::~ConnectionPair() {
    InitiateClose("Destructor");
}

bool ConnectionPair::IsValidTransition(ConnectionState from, ConnectionState to) noexcept {
    switch (from) {
        case ConnectionState::Init:
            return to == ConnectionState::ConnectingBackend ||
                   to == ConnectionState::Established ||
                   to == ConnectionState::PendingCancellation ||
                   to == ConnectionState::Closed;
        case ConnectionState::ConnectingBackend:
            return to == ConnectionState::Established ||
                   to == ConnectionState::PendingCancellation ||
                   to == ConnectionState::Closed;
        case ConnectionState::Established:
            return to == ConnectionState::HalfClosedClient ||
                   to == ConnectionState::HalfClosedBackend ||
                   to == ConnectionState::PendingCancellation ||
                   to == ConnectionState::Closed;
        case ConnectionState::HalfClosedClient:
            return to == ConnectionState::HalfClosedBackend ||
                   to == ConnectionState::PendingCancellation ||
                   to == ConnectionState::Closed;
        case ConnectionState::HalfClosedBackend:
            return to == ConnectionState::HalfClosedClient ||
                   to == ConnectionState::PendingCancellation ||
                   to == ConnectionState::Closed;
        case ConnectionState::PendingCancellation:
            return to == ConnectionState::Closed;
        case ConnectionState::Closed:
            return false;
    }
    return false;
}

bool ConnectionPair::TransitionTo(ConnectionState new_state) {
    if (state_ == new_state) {
        return true;
    }
    if (!IsValidTransition(state_, new_state)) {
        return false;
    }
    state_ = new_state;
    if (state_ == ConnectionState::Closed && pending_cqe_count_ == 0) {
        PerformCleanup();
    }
    return true;
}

void ConnectionPair::SetBackendFd(int backend_fd) noexcept {
    backend_fd_ = backend_fd;
}

void ConnectionPair::IncrementPendingCqe() noexcept {
    pending_cqe_count_++;
}

void ConnectionPair::DecrementPendingCqe() noexcept {
    if (pending_cqe_count_ > 0) {
        pending_cqe_count_--;
    }
    if (pending_cqe_count_ == 0 && state_ == ConnectionState::PendingCancellation) {
        TransitionTo(ConnectionState::Closed);
    }
}

bool ConnectionPair::HandleClientFin() {
    client_read_stopped_ = true;
    if (state_ == ConnectionState::Established) {
        if (backend_fd_ >= 0) {
            ::shutdown(backend_fd_, SHUT_WR);
        }
        TransitionTo(ConnectionState::HalfClosedClient);
        half_close_start_time_ = Clock::now();
        half_close_timer_active_ = true;
    } else if (state_ == ConnectionState::HalfClosedBackend) {
        InitiateClose("Simultaneous half-close complete");
    }
    return true;
}

bool ConnectionPair::HandleBackendFin() {
    backend_read_stopped_ = true;
    if (state_ == ConnectionState::Established) {
        if (client_fd_ >= 0) {
            ::shutdown(client_fd_, SHUT_WR);
        }
        TransitionTo(ConnectionState::HalfClosedBackend);
        half_close_start_time_ = Clock::now();
        half_close_timer_active_ = true;
    } else if (state_ == ConnectionState::HalfClosedClient) {
        InitiateClose("Simultaneous half-close complete");
    }
    return true;
}

void ConnectionPair::InitiateClose(const char* reason) {
    if (reason) std::cerr << "InitiateClose: " << reason << "\n";
    (void)reason;
    if (state_ == ConnectionState::Closed || state_ == ConnectionState::PendingCancellation) {
        return;
    }
    TransitionTo(ConnectionState::PendingCancellation);
    if (client_fd_ >= 0) {
        ::shutdown(client_fd_, SHUT_RDWR);
    }
    if (backend_fd_ >= 0) {
        ::shutdown(backend_fd_, SHUT_RDWR);
    }
    if (pending_cqe_count_ == 0) {
        TransitionTo(ConnectionState::Closed);
    }
}

bool ConnectionPair::CheckHalfCloseTimeout(TimePoint now, std::chrono::milliseconds timeout_limit) {
    if (half_close_timer_active_ && (state_ == ConnectionState::HalfClosedClient || state_ == ConnectionState::HalfClosedBackend)) {
        if ((now - half_close_start_time_) >= timeout_limit) {
            half_close_timer_active_ = false;
            InitiateClose("Half-close timeout");
            return true;
        }
    }
    return false;
}

bool ConnectionPair::CheckIdleTimeout(TimePoint now, std::chrono::milliseconds idle_limit) {
    if (state_ == ConnectionState::Established) {
        if ((now - last_active_time_) >= idle_limit) {
            InitiateClose("Idle timeout");
            return true;
        }
    }
    return false;
}

void ConnectionPair::PerformCleanup() noexcept {
    if (client_fd_ >= 0) {
        ::close(client_fd_);
        client_fd_ = -1;
    }
    if (backend_fd_ >= 0) {
        ::close(backend_fd_);
        backend_fd_ = -1;
    }
    if (cleanup_cb_) {
        auto cb = std::move(cleanup_cb_);
        cb(conn_id_);
    }
}

} // namespace helios
