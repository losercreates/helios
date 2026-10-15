#pragma once

#include <thread>
#include <atomic>
#include <array>
#include <chrono>
#include <vector>
#include <mutex>
#include <algorithm>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

namespace helios {

class TestTcpBackend {
public:
    TestTcpBackend() = default;
    ~TestTcpBackend() { Stop(); }

    bool Start() {
        if (running_) return true;
        
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(port_); // 0 for Ephemeral port by default

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd);
            return false;
        }

        socklen_t len = sizeof(addr);
        getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);

        struct timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 20000; // 20ms timeout so worker thread exits accept() cleanly on stop
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (listen(fd, 1024) < 0) {
            close(fd);
            return false;
        }

        listen_fd_.store(fd);
        running_ = true;
        worker_thread_ = std::thread([this]() { RunEchoLoop(); });
        return true;
    }

    void Stop() {
        if (running_) {
            running_ = false;
            int fd = listen_fd_.exchange(-1);
            if (fd >= 0) {
                shutdown(fd, SHUT_RDWR);
                close(fd);
            }
            if (worker_thread_.joinable()) {
                worker_thread_.join();
            }

            std::vector<std::thread> threads_to_join;
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                for (int cfd : active_clients_) {
                    shutdown(cfd, SHUT_RDWR);
                }
                threads_to_join = std::move(client_threads_);
            }
            for (auto& t : threads_to_join) {
                if (t.joinable()) {
                    t.join();
                }
            }
        }
    }

    void SetPort(uint16_t port) { port_ = port; }
    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }
    [[nodiscard]] size_t GetAcceptedCount() const noexcept { return accepted_count_; }
    void ResetAcceptedCount() noexcept { accepted_count_.store(0); }
    [[nodiscard]] size_t GetEchoCount() const noexcept { return echo_count_; }
    void ResetEchoCount() noexcept { echo_count_.store(0); }
    [[nodiscard]] bool IsRunning() const noexcept { return running_; }

private:
    void RunEchoLoop() {
        while (running_) {
            int fd = listen_fd_.load();
            if (fd < 0) break;
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);
            int client_fd = accept(fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
            if (client_fd < 0) {
                if (!running_) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            accepted_count_++;

            // Handle connection echo loop in tracked thread per connection
            std::lock_guard<std::mutex> lock(clients_mutex_);
            active_clients_.push_back(client_fd);
            client_threads_.emplace_back([this, client_fd]() {
                // Clear the 20ms accept timeout on accepted connection so active streaming connections don't drop
                struct timeval tv_conn{};
                tv_conn.tv_sec = 10;
                tv_conn.tv_usec = 0;
                setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv_conn, sizeof(tv_conn));
                setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv_conn, sizeof(tv_conn));

                int opt = 1;
                setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

                std::array<char, 16384> buf{};
                while (running_) {
                    ssize_t bytes_read = recv(client_fd, buf.data(), buf.size(), 0);
                    if (bytes_read == 0) break;
                    if (bytes_read < 0) {
                        if (errno == EINTR) continue;
                        if ((errno == EAGAIN || errno == EWOULDBLOCK) && running_) continue;
                        break;
                    }
                    echo_count_++;

                    size_t total_sent = 0;
                    bool write_failed = false;
                    while (total_sent < static_cast<size_t>(bytes_read)) {
                        ssize_t bytes_sent = send(client_fd, buf.data() + total_sent,
                                                  static_cast<size_t>(bytes_read) - total_sent, MSG_NOSIGNAL);
                        if (bytes_sent <= 0) {
                            if (bytes_sent < 0 && (errno == EINTR || ((errno == EAGAIN || errno == EWOULDBLOCK) && running_))) {
                                continue;
                            }
                            write_failed = true;
                            break;
                        }
                        total_sent += static_cast<size_t>(bytes_sent);
                    }
                    if (write_failed) break;
                }
                shutdown(client_fd, SHUT_RDWR);
                close(client_fd);
                {
                    std::lock_guard<std::mutex> lk(clients_mutex_);
                    auto it = std::find(active_clients_.begin(), active_clients_.end(), client_fd);
                    if (it != active_clients_.end()) {
                        active_clients_.erase(it);
                    }
                }
            });
        }
    }

    std::atomic<int> listen_fd_{-1};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    std::atomic<size_t> accepted_count_{0};
    std::atomic<size_t> echo_count_{0};

    std::mutex clients_mutex_;
    std::vector<int> active_clients_;
    std::vector<std::thread> client_threads_;
};

} // namespace helios
