#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <memory>
#include <chrono>

namespace helios::test {

enum class BackendBehavior {
    ECHO,
    FIXED_RESPONSE,
    DISCONNECT_ON_CONNECT,
    DISCONNECT_AFTER_READ,
    DISCONNECT_DURING_RESPONSE,
    HALF_CLOSE_AFTER_READ,
    DELAY_READ,
    DELAY_WRITE,
    IDLE,
    SEND_FRAGMENTS
};

struct BackendConfig {
    BackendBehavior behavior = BackendBehavior::ECHO;
    std::string fixed_response = "";
    std::chrono::milliseconds delay{0};
    size_t fragment_size = 1024;
    std::chrono::milliseconds fragment_delay{0};
};

class TestBackend {
public:
    TestBackend(BackendConfig config = BackendConfig{});
    ~TestBackend();

    bool Start();
    void Stop();

    uint16_t GetPort() const;
    size_t GetAcceptedCount() const;
    size_t GetTotalBytesReceived() const;
    size_t GetTotalBytesSent() const;

    void SetConfig(BackendConfig config);

private:
    void AcceptLoop();
    void HandleConnection(int client_fd);

    BackendConfig config_;
    std::atomic<int> listen_fd_{-1};
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    std::atomic<size_t> accepted_count_{0};
    std::atomic<size_t> bytes_received_{0};
    std::atomic<size_t> bytes_sent_{0};
    
    std::mutex clients_mutex_;
    std::vector<int> active_clients_;
};

} // namespace helios::test
