#include "prometheus_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cstring>
#include <sstream>
#include <vector>

namespace helios {

PrometheusServer::PrometheusServer()
    : PrometheusServer(std::make_shared<MetricsCollector>(), Config{}) {}

PrometheusServer::PrometheusServer(std::shared_ptr<MetricsCollector> collector)
    : PrometheusServer(std::move(collector), Config{}) {}

PrometheusServer::PrometheusServer(std::shared_ptr<MetricsCollector> collector, Config config)
    : config_(std::move(config)), collector_(std::move(collector)) {}

PrometheusServer::~PrometheusServer() {
    Stop();
}

bool PrometheusServer::Start() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (running_.load(std::memory_order_relaxed)) {
        return true;
    }

    int l_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (l_fd < 0) return false;

    int opt = 1;
    ::setsockopt(l_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(l_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) <= 0) {
        ::close(l_fd);
        return false;
    }

    if (::bind(l_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(l_fd);
        return false;
    }

    if (::listen(l_fd, config_.backlog) < 0) {
        ::close(l_fd);
        return false;
    }

    // Resolve assigned port if ephemeral (port == 0)
    sockaddr_in bound_addr{};
    socklen_t bound_len = sizeof(bound_addr);
    if (::getsockname(l_fd, reinterpret_cast<sockaddr*>(&bound_addr), &bound_len) == 0) {
        listen_port_ = ntohs(bound_addr.sin_port);
    } else {
        listen_port_ = config_.port;
    }

    int ep_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (ep_fd < 0) {
        ::close(l_fd);
        return false;
    }

    int s_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (s_fd < 0) {
        ::close(l_fd);
        ::close(ep_fd);
        return false;
    }

    epoll_event stop_ev{};
    stop_ev.events = EPOLLIN;
    stop_ev.data.fd = s_fd;
    if (::epoll_ctl(ep_fd, EPOLL_CTL_ADD, s_fd, &stop_ev) < 0) {
        ::close(l_fd);
        ::close(s_fd);
        ::close(ep_fd);
        return false;
    }

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

    worker_thread_ = std::thread([this]() {
        EventLoop();
    });

    return true;
}

void PrometheusServer::Stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        int s_fd = stop_event_fd_.load(std::memory_order_relaxed);
        if (s_fd >= 0) {
            uint64_t val = 1;
            [[maybe_unused]] auto res = ::write(s_fd, &val, sizeof(val));
        }
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    CleanupLocked();
}

void PrometheusServer::CleanupLocked() noexcept {
    int l_fd = listen_fd_.exchange(-1, std::memory_order_acq_rel);
    int ep_fd = epoll_fd_.load(std::memory_order_relaxed);

    if (l_fd >= 0) {
        if (ep_fd >= 0) {
            ::epoll_ctl(ep_fd, EPOLL_CTL_DEL, l_fd, nullptr);
        }
        ::close(l_fd);
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

void PrometheusServer::EventLoop() {
    int ep_fd = epoll_fd_.load(std::memory_order_relaxed);
    if (ep_fd < 0) return;

    std::array<epoll_event, 32> events{};

    while (running_.load(std::memory_order_relaxed)) {
        int nfds = ::epoll_wait(ep_fd, events.data(), static_cast<int>(events.size()), 100);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[static_cast<size_t>(i)].data.fd;
            uint32_t ev = events[static_cast<size_t>(i)].events;

            int s_fd = stop_event_fd_.load(std::memory_order_relaxed);
            if (fd == s_fd) {
                uint64_t val = 0;
                [[maybe_unused]] auto res = ::read(s_fd, &val, sizeof(val));
                return;
            }

            int l_fd = listen_fd_.load(std::memory_order_relaxed);
            if (fd == l_fd && (ev & EPOLLIN)) {
                while (true) {
                    sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = ::accept4(l_fd, reinterpret_cast<sockaddr*>(&client_addr),
                                              &client_len, SOCK_CLOEXEC);
                    if (client_fd < 0) {
                        break;
                    }
                    HandleClient(client_fd);
                }
            }
        }
    }
}

void PrometheusServer::HandleClient(int client_fd) {
    // Set socket receive timeout so slow/hanging connections don't block
    struct timeval tv{};
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int opt = 1;
    ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    std::array<char, 4096> buf{};
    ssize_t n = ::recv(client_fd, buf.data(), buf.size() - 1, 0);
    if (n <= 0) {
        ::close(client_fd);
        return;
    }
    buf[static_cast<size_t>(n)] = '\0';

    std::string_view req(buf.data(), static_cast<size_t>(n));

    // Simple HTTP method and target extraction
    bool is_get = req.starts_with("GET ");
    std::string_view path;
    if (is_get) {
        size_t path_start = 4;
        size_t path_end = req.find(' ', path_start);
        if (path_end != std::string_view::npos) {
            path = req.substr(path_start, path_end - path_start);
        }
    }

    std::string response;
    if (!is_get) {
        response = "HTTP/1.1 405 Method Not Allowed\r\n"
                   "Content-Type: text/plain; charset=utf-8\r\n"
                   "Content-Length: 18\r\n"
                   "Connection: close\r\n"
                   "\r\n"
                   "Method Not Allowed";
    } else if (path == "/metrics") {
        std::string body = collector_ ? collector_->RenderPrometheusText() : "";
        std::ostringstream ss;
        ss << "HTTP/1.1 200 OK\r\n"
           << "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n"
           << "\r\n"
           << body;
        response = ss.str();
    } else if (path == "/healthz") {
        std::string body = "OK";
        std::ostringstream ss;
        ss << "HTTP/1.1 200 OK\r\n"
           << "Content-Type: text/plain; charset=utf-8\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n"
           << "\r\n"
           << body;
        response = ss.str();
    } else {
        std::string body = "Not Found";
        std::ostringstream ss;
        ss << "HTTP/1.1 404 Not Found\r\n"
           << "Content-Type: text/plain; charset=utf-8\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n"
           << "\r\n"
           << body;
        response = ss.str();
    }

    size_t total_sent = 0;
    while (total_sent < response.size()) {
        ssize_t sent = ::send(client_fd, response.data() + total_sent,
                              response.size() - total_sent, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (sent < 0 && errno == EINTR) continue;
            break;
        }
        total_sent += static_cast<size_t>(sent);
    }

    ::shutdown(client_fd, SHUT_RDWR);
    ::close(client_fd);
}

} // namespace helios
