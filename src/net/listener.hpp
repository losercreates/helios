#pragma once

#include <string>
#include <cstdint>
#include <atomic>
#include <netinet/in.h>

namespace helios {

struct ListenerConfig {
    std::string host{"127.0.0.1"};
    uint16_t port{8080};
    int backlog{128};
    bool reuse_port{true};
    bool reuse_addr{true};
    bool non_blocking{true};
};

// Manages a multi-socket listening file descriptor with SO_REUSEPORT support for multi-worker scaling
class Listener {
public:
    Listener() = default;
    explicit Listener(ListenerConfig config);
    ~Listener();

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    Listener(Listener&& other) noexcept;
    Listener& operator=(Listener&& other) noexcept;

    bool BindAndListen();
    bool BindAndListen(const ListenerConfig& config);
    void Close() noexcept;

    [[nodiscard]] int GetFd() const noexcept { return fd_; }
    [[nodiscard]] uint16_t GetPort() const noexcept { return port_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool IsListening() const noexcept { return fd_ >= 0; }
    [[nodiscard]] const ListenerConfig& GetConfig() const noexcept { return config_; }

private:
    ListenerConfig config_;
    int fd_{-1};
    std::atomic<uint16_t> port_{0};
};

} // namespace helios
