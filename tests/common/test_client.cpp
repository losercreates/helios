#include "test_client.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <stdexcept>

namespace helios::test {

TestClient::TestClient() = default;

TestClient::~TestClient() {
    Disconnect();
}

bool TestClient::Connect(const std::string& host, uint16_t port, std::chrono::milliseconds timeout) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    // Set non-blocking
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    int res = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (res < 0 && errno != EINPROGRESS) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    if (res == 0) {
        connected_ = true;
        fcntl(fd_, F_SETFL, flags); // Restore blocking state if needed, or leave non-blocking. Let's leave it non-blocking for timeout handling
        return true;
    }

    struct pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLOUT;

    res = poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (res > 0) {
        int error = 0;
        socklen_t len = sizeof(error);
        getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len);
        if (error == 0) {
            connected_ = true;
            fcntl(fd_, F_SETFL, flags);
            return true;
        }
    }

    close(fd_);
    fd_ = -1;
    return false;
}

void TestClient::Disconnect() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    connected_ = false;
}

void TestClient::HalfClose() {
    if (fd_ >= 0) {
        shutdown(fd_, SHUT_WR);
    }
}

ssize_t TestClient::Send(const std::vector<uint8_t>& data) {
    if (!connected_) return -1;
    return ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
}

ssize_t TestClient::Send(const std::string& data) {
    if (!connected_) return -1;
    return ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
}

ssize_t TestClient::SendFragmented(const std::string& data, size_t chunk_size, std::chrono::milliseconds delay_between_chunks) {
    if (!connected_) return -1;
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        size_t chunk = std::min(chunk_size, data.size() - total_sent);
        ssize_t n = ::send(fd_, data.data() + total_sent, chunk, MSG_NOSIGNAL);
        if (n <= 0) return total_sent > 0 ? static_cast<ssize_t>(total_sent) : n;
        total_sent += static_cast<size_t>(n);
        std::this_thread::sleep_for(delay_between_chunks);
    }
    return static_cast<ssize_t>(total_sent);
}

std::vector<uint8_t> TestClient::Receive(size_t max_bytes, std::chrono::milliseconds timeout) {
    if (!connected_) return {};
    
    struct pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;

    int res = poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (res > 0) {
        std::vector<uint8_t> buf(max_bytes);
        ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
        if (n > 0) {
            buf.resize(static_cast<size_t>(n));
            return buf;
        } else if (n == 0) {
            connected_ = false; // Closed by peer
        }
    }
    return {};
}

std::string TestClient::ReceiveString(size_t max_bytes, std::chrono::milliseconds timeout) {
    auto data = Receive(max_bytes, timeout);
    return std::string(data.begin(), data.end());
}

std::vector<uint8_t> TestClient::ReceiveExact(size_t exact_bytes, std::chrono::milliseconds timeout) {
    if (!connected_) return {};
    
    std::vector<uint8_t> result;
    result.reserve(exact_bytes);
    
    auto start_time = std::chrono::steady_clock::now();
    
    while (result.size() < exact_bytes) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
        if (elapsed >= timeout) break;
        
        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;

        int res = poll(&pfd, 1, static_cast<int>((timeout - elapsed).count()));
        if (res > 0) {
            std::vector<uint8_t> buf(exact_bytes - result.size());
            ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
            if (n > 0) {
                result.insert(result.end(), buf.begin(), buf.begin() + n);
            } else if (n == 0) {
                connected_ = false;
                break;
            }
        } else if (res == 0) {
            break; // timeout
        }
    }
    return result;
}

std::vector<uint8_t> TestClient::ReceiveUntilClose(std::chrono::milliseconds timeout) {
    if (!connected_) return {};
    
    std::vector<uint8_t> result;
    auto start_time = std::chrono::steady_clock::now();
    
    while (true) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
        if (elapsed >= timeout) break;
        
        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;

        int res = poll(&pfd, 1, static_cast<int>((timeout - elapsed).count()));
        if (res > 0) {
            std::vector<uint8_t> buf(8192);
            ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
            if (n > 0) {
                result.insert(result.end(), buf.begin(), buf.begin() + n);
            } else if (n == 0) {
                connected_ = false;
                break;
            } else {
                break; // error
            }
        } else if (res == 0) {
            break; // timeout
        }
    }
    return result;
}

bool TestClient::IsConnected() const {
    return connected_;
}

} // namespace helios::test
