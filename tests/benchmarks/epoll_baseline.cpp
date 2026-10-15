#include "epoll_baseline.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>

namespace {

constexpr uint32_t kEpollIn = EPOLLIN;
constexpr uint32_t kEpollOut = EPOLLOUT;

constexpr uint32_t EpollOutIf(bool condition) noexcept {
    return condition ? kEpollOut : 0u;
}

constexpr uint32_t EpollInIf(bool condition) noexcept {
    return condition ? kEpollIn : 0u;
}

} // namespace

namespace helios {

EpollBaselineProxy::EpollBaselineProxy()
    : EpollBaselineProxy(Config{}) {
}

EpollBaselineProxy::EpollBaselineProxy(Config config)
    : config_(std::move(config)),
      buffer_pool_(config_.total_buffers, config_.buffer_size),
      lb_(std::make_shared<RoundRobinLoadBalancer>()) {
}

EpollBaselineProxy::~EpollBaselineProxy() {
    Stop();
}

void EpollBaselineProxy::AddBackend(std::shared_ptr<Backend> backend) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (lb_ && backend) {
        lb_->AddBackend(std::move(backend));
    }
}

void EpollBaselineProxy::AddBackend(std::string host, uint16_t port, uint32_t weight) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (lb_) {
        auto b = std::make_shared<Backend>(static_cast<uint32_t>(lb_->GetBackendCount() + 1), std::move(host), port, weight);
        lb_->AddBackend(std::move(b));
    }
}

void EpollBaselineProxy::SetLoadBalancer(std::shared_ptr<LoadBalancer> lb) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    lb_ = std::move(lb);
}

size_t EpollBaselineProxy::GetActiveConnectionCount() const noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return connections_.size();
}

bool EpollBaselineProxy::Start() {
    if (running_.load(std::memory_order_relaxed)) {
        return true;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (running_.load(std::memory_order_relaxed)) {
        return true;
    }

    // 1. Create epoll instance
    int ep_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (ep_fd < 0) {
        return false;
    }

    // 2. Create stop eventfd
    int s_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (s_fd < 0) {
        ::close(ep_fd);
        return false;
    }

    epoll_event stop_ev{};
    stop_ev.events = EPOLLIN;
    stop_ev.data.fd = s_fd;
    if (::epoll_ctl(ep_fd, EPOLL_CTL_ADD, s_fd, &stop_ev) < 0) {
        ::close(s_fd);
        ::close(ep_fd);
        return false;
    }

    // 3. Create non-blocking listening socket
    int l_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (l_fd < 0) {
        ::close(s_fd);
        ::close(ep_fd);
        return false;
    }

    int opt = 1;
    ::setsockopt(l_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.listen_port);
    if (::inet_pton(AF_INET, config_.listen_host.c_str(), &addr.sin_addr) <= 0) {
        ::close(l_fd);
        ::close(s_fd);
        ::close(ep_fd);
        return false;
    }

    if (::bind(l_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(l_fd);
        ::close(s_fd);
        ::close(ep_fd);
        return false;
    }

    if (::listen(l_fd, 1024) < 0) {
        ::close(l_fd);
        ::close(s_fd);
        ::close(ep_fd);
        return false;
    }

    // Resolve assigned port if ephemeral (port == 0)
    sockaddr_in bound_addr{};
    socklen_t addr_len = sizeof(bound_addr);
    if (::getsockname(l_fd, reinterpret_cast<sockaddr*>(&bound_addr), &addr_len) == 0) {
        listen_port_ = ntohs(bound_addr.sin_port);
    } else {
        listen_port_ = config_.listen_port;
    }

    // Register listening socket with epoll
    epoll_event listen_ev{};
    listen_ev.events = EPOLLIN;
    listen_ev.data.fd = l_fd;
    if (::epoll_ctl(ep_fd, EPOLL_CTL_ADD, l_fd, &listen_ev) < 0) {
        ::close(l_fd);
        ::close(s_fd);
        ::close(ep_fd);
        return false;
    }

    epoll_fd_.store(ep_fd, std::memory_order_release);
    stop_event_fd_.store(s_fd, std::memory_order_release);
    listen_fd_.store(l_fd, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    return true;
}

void EpollBaselineProxy::Stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    // Wake up epoll_wait if blocked
    int s_fd = stop_event_fd_.load(std::memory_order_relaxed);
    if (s_fd >= 0) {
        uint64_t val = 1;
        [[maybe_unused]] auto res = ::write(s_fd, &val, sizeof(val));
    }

    // If Run() is active on another thread, that thread will invoke Cleanup() upon loop exit.
    // If no runner thread is active, clean up directly.
    if (runner_thread_id_.load(std::memory_order_acquire) == std::thread::id{}) {
        Cleanup();
    }
}

void EpollBaselineProxy::Run() {
    runner_thread_id_.store(std::this_thread::get_id(), std::memory_order_release);
    while (running_.load(std::memory_order_relaxed)) {
        Step(100);
    }
    Cleanup();
    runner_thread_id_.store(std::thread::id{}, std::memory_order_release);
}

void EpollBaselineProxy::Cleanup() noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);

    int l_fd = listen_fd_.exchange(-1, std::memory_order_acq_rel);
    int ep_fd = epoll_fd_.load(std::memory_order_relaxed);

    // Close listening socket
    if (l_fd >= 0) {
        if (ep_fd >= 0) {
            ::epoll_ctl(ep_fd, EPOLL_CTL_DEL, l_fd, nullptr);
        }
        ::close(l_fd);
    }

    // Close all active client and backend connections
    std::vector<uint64_t> conn_ids;
    conn_ids.reserve(connections_.size());
    for (const auto& [cid, _] : connections_) {
        conn_ids.push_back(cid);
    }
    for (uint64_t cid : conn_ids) {
        CloseConnection(cid);
    }

    int s_fd = stop_event_fd_.exchange(-1, std::memory_order_acq_rel);
    if (s_fd >= 0) {
        ::close(s_fd);
    }

    ep_fd = epoll_fd_.exchange(-1, std::memory_order_acq_rel);
    if (ep_fd >= 0) {
        ::close(ep_fd);
    }
}

int EpollBaselineProxy::Step(int timeout_ms) {
    int ep_fd = epoll_fd_.load(std::memory_order_relaxed);
    if (ep_fd < 0) {
        return -1;
    }

    std::vector<epoll_event> events(static_cast<size_t>(config_.max_events));
    int nfds = ::epoll_wait(ep_fd, events.data(), config_.max_events, timeout_ms);
    if (nfds < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!running_.load(std::memory_order_relaxed)) {
        return 0;
    }

    int l_fd = listen_fd_.load(std::memory_order_relaxed);
    int s_fd = stop_event_fd_.load(std::memory_order_relaxed);

    for (int i = 0; i < nfds; ++i) {
        int fd = events[static_cast<size_t>(i)].data.fd;
        uint32_t ev = events[static_cast<size_t>(i)].events;

        if (fd == s_fd) {
            uint64_t val = 0;
            [[maybe_unused]] auto res = ::read(s_fd, &val, sizeof(val));
            continue;
        }

        if (fd == l_fd) {
            HandleAccept();
        } else {
            HandleSocketEvent(fd, ev);
        }
    }

    return nfds;
}

void EpollBaselineProxy::HandleAccept() {
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // No more incoming connections
            }
            break;
        }

        if (!lb_) {
            ::close(client_fd);
            continue;
        }

        auto backend = lb_->SelectBackend();
        if (!backend) {
            ::close(client_fd);
            continue;
        }

        // Create non-blocking backend socket
        int backend_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (backend_fd < 0) {
            ::close(client_fd);
            continue;
        }

        // Disable Nagle's algorithm for benchmark latency parity
        int flag = 1;
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        ::setsockopt(backend_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        const auto& baddr = backend->GetSockAddr();
        int connect_res = ::connect(backend_fd, reinterpret_cast<const sockaddr*>(&baddr), sizeof(baddr));

        uint64_t cid = next_conn_id_++;
        ++total_accepted_;

        auto conn = std::make_unique<ConnectionState>();
        conn->conn_id = cid;
        conn->client_fd = client_fd;
        conn->backend_fd = backend_fd;
        conn->backend = backend;
        backend->IncrementActiveConnections();

        if (connect_res == 0) {
            conn->backend_connecting = false;
        } else if (errno == EINPROGRESS) {
            conn->backend_connecting = true;
        } else {
            // Immediate connection failure
            backend->DecrementActiveConnections();
            backend->ReportConnectionFailure();
            ::close(client_fd);
            ::close(backend_fd);
            continue;
        }

        fd_to_conn_id_[client_fd] = cid;
        fd_to_conn_id_[backend_fd] = cid;

        // Register client in epoll for reads
        epoll_event ev_client{};
        ev_client.events = EPOLLIN | EPOLLERR | EPOLLHUP;
        ev_client.data.fd = client_fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev_client);

        // Register backend in epoll
        epoll_event ev_backend{};
        if (conn->backend_connecting) {
            ev_backend.events = EPOLLOUT | EPOLLERR | EPOLLHUP;
        } else {
            ev_backend.events = EPOLLIN | EPOLLERR | EPOLLHUP;
        }
        ev_backend.data.fd = backend_fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, backend_fd, &ev_backend);

        connections_[cid] = std::move(conn);
    }
}

void EpollBaselineProxy::UpdateEpollInterest(int fd, uint32_t events) {
    int ep_fd = epoll_fd_.load(std::memory_order_relaxed);
    if (ep_fd < 0 || fd < 0) return;
    epoll_event ev{};
    ev.events = events | EPOLLERR | EPOLLHUP;
    ev.data.fd = fd;
    ::epoll_ctl(ep_fd, EPOLL_CTL_MOD, fd, &ev);
}

void EpollBaselineProxy::HandleSocketEvent(int fd, uint32_t events) {
    auto it = fd_to_conn_id_.find(fd);
    if (it == fd_to_conn_id_.end()) {
        return;
    }

    uint64_t cid = it->second;
    auto conn_it = connections_.find(cid);
    if (conn_it == connections_.end()) {
        return;
    }

    ConnectionState* conn = conn_it->second.get();

    // 1. Check if backend is in the middle of connecting
    if (fd == conn->backend_fd && conn->backend_connecting) {
        HandleBackendConnect(*conn);
        return;
    }

    // 2. Handle socket errors
    if (events & EPOLLERR) {
        CloseConnection(cid);
        return;
    }

    // 3. Handle hang-up (remote peer closed write or connection terminated)
    if (events & EPOLLHUP) {
        if (fd == conn->client_fd) {
            conn->client_read_closed = true;
            if (conn->c2b_buf == nullptr || conn->c2b_buf->ReadableBytes() == 0) {
                ::shutdown(conn->backend_fd, SHUT_WR);
            }
        } else if (fd == conn->backend_fd) {
            conn->backend_read_closed = true;
            if (conn->b2c_buf == nullptr || conn->b2c_buf->ReadableBytes() == 0) {
                ::shutdown(conn->client_fd, SHUT_WR);
            }
        }
    }

    // 4. Process writable events
    if (events & EPOLLOUT) {
        if (fd == conn->backend_fd) {
            HandleBackendWrite(*conn);
        } else if (fd == conn->client_fd) {
            HandleClientWrite(*conn);
        }
    }

    // Check if connection was closed during write
    if (connections_.find(cid) == connections_.end()) {
        return;
    }

    // 5. Process readable events
    if (events & EPOLLIN) {
        if (fd == conn->client_fd) {
            HandleClientRead(*conn);
        } else if (fd == conn->backend_fd) {
            HandleBackendRead(*conn);
        }
    }

    // Check if connection was closed during read
    if (connections_.find(cid) == connections_.end()) {
        return;
    }

    // 6. Check if both sides are done and all buffers flushed
    if (conn->client_read_closed && conn->backend_read_closed &&
        conn->c2b_buf == nullptr && conn->b2c_buf == nullptr) {
        CloseConnection(cid);
    }
}

void EpollBaselineProxy::HandleBackendConnect(ConnectionState& conn) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(conn.backend_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
        // Connection refused or timed out
        if (conn.backend) {
            conn.backend->ReportConnectionFailure();
        }
        CloseConnection(conn.conn_id);
        return;
    }

    conn.backend_connecting = false;
    if (conn.backend) {
        conn.backend->ReportConnectionSuccess();
    }

    // Switch backend to read mode, and write mode if client already sent data
    uint32_t b_events = EPOLLIN;
    if (conn.c2b_buf != nullptr && conn.c2b_buf->ReadableBytes() > 0) {
        b_events |= EPOLLOUT;
    }
    UpdateEpollInterest(conn.backend_fd, b_events);

    if (conn.c2b_buf != nullptr && conn.c2b_buf->ReadableBytes() > 0) {
        HandleBackendWrite(conn);
    }
}

void EpollBaselineProxy::HandleClientRead(ConnectionState& conn) {
    if (conn.c2b_buf == nullptr) {
        conn.c2b_buf = buffer_pool_.Acquire();
        if (conn.c2b_buf == nullptr) {
            // Backpressure: pause client read
            UpdateEpollInterest(conn.client_fd, EpollOutIf(conn.b2c_buf != nullptr));
            return;
        }
    }

    while (conn.c2b_buf->WritableBytes() > 0) {
        ssize_t n = ::read(conn.client_fd, conn.c2b_buf->WritePtr(), conn.c2b_buf->WritableBytes());
        if (n > 0) {
            conn.c2b_buf->AdvanceWrite(static_cast<size_t>(n));
            // Trigger backend write
            if (!conn.backend_connecting) {
                UpdateEpollInterest(conn.backend_fd, kEpollIn | kEpollOut);
            }
            if (conn.c2b_buf->WritableBytes() == 0) {
                // Buffer full, pause reading client
                UpdateEpollInterest(conn.client_fd, EpollOutIf(conn.b2c_buf != nullptr));
                break;
            }
        } else if (n == 0) {
            // Client EOF
            conn.client_read_closed = true;
            if (conn.c2b_buf != nullptr && conn.c2b_buf->ReadableBytes() == 0) {
                buffer_pool_.Release(conn.c2b_buf);
                conn.c2b_buf = nullptr;
            }
            UpdateEpollInterest(conn.client_fd, EpollOutIf(conn.b2c_buf != nullptr));
            if (conn.c2b_buf == nullptr) {
                ::shutdown(conn.backend_fd, SHUT_WR);
            }
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (conn.c2b_buf != nullptr && conn.c2b_buf->ReadableBytes() == 0) {
                    buffer_pool_.Release(conn.c2b_buf);
                    conn.c2b_buf = nullptr;
                }
                break;
            }
            // Error
            CloseConnection(conn.conn_id);
            return;
        }
    }
}

void EpollBaselineProxy::HandleBackendWrite(ConnectionState& conn) {
    if (conn.c2b_buf == nullptr || conn.c2b_buf->ReadableBytes() == 0) {
        if (conn.c2b_buf != nullptr) {
            buffer_pool_.Release(conn.c2b_buf);
            conn.c2b_buf = nullptr;
        }
        UpdateEpollInterest(conn.backend_fd, kEpollIn);
        if (conn.client_read_closed) {
            ::shutdown(conn.backend_fd, SHUT_WR);
        }
        return;
    }

    while (conn.c2b_buf->ReadableBytes() > 0) {
        ssize_t n = ::send(conn.backend_fd, conn.c2b_buf->ReadPtr(), conn.c2b_buf->ReadableBytes(), MSG_NOSIGNAL);
        if (n > 0) {
            conn.c2b_buf->AdvanceRead(static_cast<size_t>(n));
            if (conn.c2b_buf->ReadableBytes() == 0) {
                buffer_pool_.Release(conn.c2b_buf);
                conn.c2b_buf = nullptr;
                UpdateEpollInterest(conn.backend_fd, kEpollIn);

                // Resume reading from client if not closed
                if (!conn.client_read_closed) {
                    UpdateEpollInterest(conn.client_fd, kEpollIn | EpollOutIf(conn.b2c_buf != nullptr));
                } else {
                    ::shutdown(conn.backend_fd, SHUT_WR);
                }
                break;
            }
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            CloseConnection(conn.conn_id);
            return;
        }
    }
}

void EpollBaselineProxy::HandleBackendRead(ConnectionState& conn) {
    if (conn.b2c_buf == nullptr) {
        conn.b2c_buf = buffer_pool_.Acquire();
        if (conn.b2c_buf == nullptr) {
            // Backpressure: pause backend read
            UpdateEpollInterest(conn.backend_fd, EpollOutIf(conn.c2b_buf != nullptr));
            return;
        }
    }

    while (conn.b2c_buf->WritableBytes() > 0) {
        ssize_t n = ::read(conn.backend_fd, conn.b2c_buf->WritePtr(), conn.b2c_buf->WritableBytes());
        if (n > 0) {
            conn.b2c_buf->AdvanceWrite(static_cast<size_t>(n));
            UpdateEpollInterest(conn.client_fd, kEpollIn | kEpollOut);
            if (conn.b2c_buf->WritableBytes() == 0) {
                // Buffer full, pause backend read
                UpdateEpollInterest(conn.backend_fd, EpollOutIf(conn.c2b_buf != nullptr));
                break;
            }
        } else if (n == 0) {
            // Backend EOF
            conn.backend_read_closed = true;
            if (conn.b2c_buf != nullptr && conn.b2c_buf->ReadableBytes() == 0) {
                buffer_pool_.Release(conn.b2c_buf);
                conn.b2c_buf = nullptr;
            }
            UpdateEpollInterest(conn.backend_fd, EpollOutIf(conn.c2b_buf != nullptr));
            if (conn.b2c_buf == nullptr) {
                ::shutdown(conn.client_fd, SHUT_WR);
            }
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (conn.b2c_buf != nullptr && conn.b2c_buf->ReadableBytes() == 0) {
                    buffer_pool_.Release(conn.b2c_buf);
                    conn.b2c_buf = nullptr;
                }
                break;
            }
            CloseConnection(conn.conn_id);
            return;
        }
    }
}

void EpollBaselineProxy::HandleClientWrite(ConnectionState& conn) {
    if (conn.b2c_buf == nullptr || conn.b2c_buf->ReadableBytes() == 0) {
        if (conn.b2c_buf != nullptr) {
            buffer_pool_.Release(conn.b2c_buf);
            conn.b2c_buf = nullptr;
        }
        UpdateEpollInterest(conn.client_fd, EpollInIf(!conn.client_read_closed));
        if (conn.backend_read_closed) {
            ::shutdown(conn.client_fd, SHUT_WR);
        }
        return;
    }

    while (conn.b2c_buf->ReadableBytes() > 0) {
        ssize_t n = ::send(conn.client_fd, conn.b2c_buf->ReadPtr(), conn.b2c_buf->ReadableBytes(), MSG_NOSIGNAL);
        if (n > 0) {
            conn.b2c_buf->AdvanceRead(static_cast<size_t>(n));
            if (conn.b2c_buf->ReadableBytes() == 0) {
                buffer_pool_.Release(conn.b2c_buf);
                conn.b2c_buf = nullptr;
                UpdateEpollInterest(conn.client_fd, EpollInIf(!conn.client_read_closed));

                // Resume reading from backend if not closed
                if (!conn.backend_read_closed) {
                    UpdateEpollInterest(conn.backend_fd, kEpollIn | EpollOutIf(conn.c2b_buf != nullptr));
                } else {
                    ::shutdown(conn.client_fd, SHUT_WR);
                }
                break;
            }
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            CloseConnection(conn.conn_id);
            return;
        }
    }
}

void EpollBaselineProxy::CloseConnection(uint64_t conn_id) {
    auto it = connections_.find(conn_id);
    if (it == connections_.end()) {
        return;
    }

    auto conn = std::move(it->second);
    connections_.erase(it);

    if (conn->backend) {
        conn->backend->DecrementActiveConnections();
    }

    int ep_fd = epoll_fd_.load(std::memory_order_relaxed);
    if (ep_fd >= 0) {
        if (conn->client_fd >= 0) {
            ::epoll_ctl(ep_fd, EPOLL_CTL_DEL, conn->client_fd, nullptr);
        }
        if (conn->backend_fd >= 0) {
            ::epoll_ctl(ep_fd, EPOLL_CTL_DEL, conn->backend_fd, nullptr);
        }
    }

    if (conn->client_fd >= 0) {
        fd_to_conn_id_.erase(conn->client_fd);
        ::close(conn->client_fd);
        conn->client_fd = -1;
    }

    if (conn->backend_fd >= 0) {
        fd_to_conn_id_.erase(conn->backend_fd);
        ::close(conn->backend_fd);
        conn->backend_fd = -1;
    }

    if (conn->c2b_buf) {
        buffer_pool_.Release(conn->c2b_buf);
        conn->c2b_buf = nullptr;
    }

    if (conn->b2c_buf) {
        buffer_pool_.Release(conn->b2c_buf);
        conn->b2c_buf = nullptr;
    }
}

} // namespace helios
