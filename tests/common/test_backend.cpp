#include "test_backend.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <algorithm>

namespace helios::test {

TestBackend::TestBackend(BackendConfig config) : config_(std::move(config)) {}

TestBackend::~TestBackend() {
    Stop();
}

bool TestBackend::Start() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0; // Ephemeral

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);

    if (listen(fd, 1024) < 0) {
        close(fd);
        return false;
    }

    listen_fd_.store(fd);
    running_ = true;
    accept_thread_ = std::thread(&TestBackend::AcceptLoop, this);
    return true;
}

void TestBackend::Stop() {
    if (running_.exchange(false)) {
        int fd = listen_fd_.exchange(-1);
        if (fd >= 0) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }

        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (int cfd : active_clients_) {
            shutdown(cfd, SHUT_RDWR);
            close(cfd);
        }
        active_clients_.clear();
    }
}

uint16_t TestBackend::GetPort() const { return port_; }
size_t TestBackend::GetAcceptedCount() const { return accepted_count_.load(); }
size_t TestBackend::GetTotalBytesReceived() const { return bytes_received_.load(); }
size_t TestBackend::GetTotalBytesSent() const { return bytes_sent_.load(); }

void TestBackend::SetConfig(BackendConfig config) {
    config_ = std::move(config);
}

void TestBackend::AcceptLoop() {
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

        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            active_clients_.push_back(client_fd);
        }

        if (config_.behavior == BackendBehavior::DISCONNECT_ON_CONNECT) {
            close(client_fd);
            continue;
        }

        std::thread(&TestBackend::HandleConnection, this, client_fd).detach();
    }
}

void TestBackend::HandleConnection(int client_fd) {
    std::array<char, 8192> buf;
    
    while (running_) {
        if (config_.behavior == BackendBehavior::DELAY_READ && config_.delay.count() > 0) {
            std::this_thread::sleep_for(config_.delay);
        }

        ssize_t n = recv(client_fd, buf.data(), buf.size(), 0);
        if (n <= 0) break;
        bytes_received_ += static_cast<size_t>(n);

        if (config_.behavior == BackendBehavior::DISCONNECT_AFTER_READ) {
            break;
        }
        
        if (config_.behavior == BackendBehavior::HALF_CLOSE_AFTER_READ) {
            shutdown(client_fd, SHUT_WR);
            continue; // Can still read, but won't write
        }
        
        if (config_.behavior == BackendBehavior::IDLE) {
            continue;
        }

        if (config_.behavior == BackendBehavior::DELAY_WRITE && config_.delay.count() > 0) {
            std::this_thread::sleep_for(config_.delay);
        }

        if (config_.behavior == BackendBehavior::ECHO || config_.behavior == BackendBehavior::DELAY_READ || config_.behavior == BackendBehavior::DELAY_WRITE) {
            ssize_t sent = 0;
            while (sent < n) {
                ssize_t res = send(client_fd, buf.data() + sent, static_cast<size_t>(n - sent), MSG_NOSIGNAL);
                if (res <= 0) goto end_conn;
                sent += res;
                bytes_sent_ += static_cast<size_t>(res);
            }
        } else if (config_.behavior == BackendBehavior::FIXED_RESPONSE) {
            ssize_t sent = 0;
            while (sent < static_cast<ssize_t>(config_.fixed_response.size())) {
                ssize_t res = send(client_fd, config_.fixed_response.data() + sent, config_.fixed_response.size() - static_cast<size_t>(sent), MSG_NOSIGNAL);
                if (res <= 0) goto end_conn;
                sent += res;
                bytes_sent_ += static_cast<size_t>(res);
            }
        } else if (config_.behavior == BackendBehavior::SEND_FRAGMENTS) {
            size_t total_len = config_.fixed_response.empty() ? static_cast<size_t>(n) : config_.fixed_response.size();
            const char* data = config_.fixed_response.empty() ? buf.data() : config_.fixed_response.data();
            
            size_t sent = 0;
            while (sent < total_len) {
                size_t chunk = std::min(config_.fragment_size, total_len - sent);
                ssize_t res = send(client_fd, data + sent, chunk, MSG_NOSIGNAL);
                if (res <= 0) goto end_conn;
                sent += static_cast<size_t>(res);
                bytes_sent_ += static_cast<size_t>(res);
                if (config_.fragment_delay.count() > 0) {
                    std::this_thread::sleep_for(config_.fragment_delay);
                }
            }
        }
        
        if (config_.behavior == BackendBehavior::DISCONNECT_DURING_RESPONSE) {
            send(client_fd, config_.fixed_response.data(), config_.fixed_response.size(), MSG_NOSIGNAL);
            break;
        }
    }

end_conn:
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = std::find(active_clients_.begin(), active_clients_.end(), client_fd);
        if (it != active_clients_.end()) {
            active_clients_.erase(it);
        }
    }
    shutdown(client_fd, SHUT_RDWR);
    close(client_fd);
}

} // namespace helios::test
