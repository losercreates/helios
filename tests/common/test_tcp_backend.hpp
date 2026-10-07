#pragma once

#include <thread>
#include <atomic>
#include <array>
#include <chrono>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
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

        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd);
            return false;
        }

        socklen_t len = sizeof(addr);
        getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);

        if (listen(fd, 128) < 0) {
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
        }
    }

    void SetPort(uint16_t port) { port_ = port; }
    [[nodiscard]] uint16_t GetPort() const noexcept { return port_; }
    [[nodiscard]] size_t GetAcceptedCount() const noexcept { return accepted_count_; }
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

            // Handle connection echo loop in background thread per connection
            std::thread([client_fd]() {
                std::array<char, 8192> buf{};
                while (true) {
                    ssize_t bytes_read = recv(client_fd, buf.data(), buf.size(), 0);
                    if (bytes_read <= 0) break;
                    ssize_t bytes_sent = send(client_fd, buf.data(), static_cast<size_t>(bytes_read), 0);
                    if (bytes_sent <= 0) break;
                }
                shutdown(client_fd, SHUT_RDWR);
                close(client_fd);
            }).detach();
        }
    }

    std::atomic<int> listen_fd_{-1};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    std::atomic<size_t> accepted_count_{0};
};

} // namespace helios
