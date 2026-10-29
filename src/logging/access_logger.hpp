#pragma once

#include <string>
#include <chrono>
#include <cstdint>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <fstream>
#include <string_view>

namespace helios {

enum class LogFormat : uint8_t {
    Json,
    Combined
};

struct AccessLogEntry {
    std::chrono::system_clock::time_point timestamp{std::chrono::system_clock::now()};
    std::string client_ip{"127.0.0.1"};
    uint16_t client_port{0};
    std::string method{"GET"};
    std::string uri{"/"};
    std::string http_version{"HTTP/1.1"};
    int status_code{200};
    uint64_t bytes_sent{0};
    uint64_t bytes_received{0};
    std::chrono::microseconds duration_us{0};
    std::string upstream_endpoint;
};

struct AccessLoggerConfig {
    bool enabled{true};
    LogFormat format{LogFormat::Json};
    std::string output{"stdout"}; // "stdout", "stderr", or file path
    size_t queue_capacity{65536};
    std::chrono::milliseconds flush_interval{100};
};

// High-performance asynchronous, non-blocking access logger.
// Dedicated background thread flushes batched records to stdout, stderr, or logfile
// ensuring worker event loop threads never block on disk I/O.
class AccessLogger {
public:
    explicit AccessLogger(AccessLoggerConfig config = {});
    ~AccessLogger();

    AccessLogger(const AccessLogger&) = delete;
    AccessLogger& operator=(const AccessLogger&) = delete;

    bool Start();
    void Stop();
    bool Reopen(const std::string& new_path = "");

    // Hot-path non-blocking push from worker threads
    void Log(AccessLogEntry entry);

    [[nodiscard]] bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t GetTotalLogged() const noexcept { return total_logged_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t GetTotalDropped() const noexcept { return total_dropped_.load(std::memory_order_relaxed); }
    [[nodiscard]] const AccessLoggerConfig& GetConfig() const noexcept { return config_; }

    // Pure formatting utilities
    static std::string FormatJson(const AccessLogEntry& entry);
    static std::string FormatCombined(const AccessLogEntry& entry);
    static std::string FormatIso8601(std::chrono::system_clock::time_point tp);

private:
    void FlushLoop();
    void WriteBuffer(const std::string& data);

    AccessLoggerConfig config_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::vector<AccessLogEntry> queue_;

    std::atomic<uint64_t> total_logged_{0};
    std::atomic<uint64_t> total_dropped_{0};

    std::thread worker_thread_;
    std::mutex output_mutex_;
    std::ofstream file_stream_;
};

} // namespace helios
