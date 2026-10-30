#pragma once

#include "io/ring_engine.hpp"
#include "io/op_context.hpp"
#include "lb/backend.hpp"
#include <memory>
#include <chrono>
#include <string>
#include <functional>

namespace helios {

enum class HealthCheckType {
    Tcp,
    Http
};

class HealthChecker {
public:
    HealthChecker(RingEngine& engine, std::shared_ptr<Backend> backend,
                  std::chrono::milliseconds interval = std::chrono::milliseconds(5000),
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(2000),
                  HealthCheckType check_type = HealthCheckType::Tcp,
                  std::string http_path = "/health",
                  uint16_t expected_status = 200);
    ~HealthChecker();

    // Prevent copy and move to ensure stable addresses for context pointers
    HealthChecker(const HealthChecker&) = delete;
    HealthChecker& operator=(const HealthChecker&) = delete;
    HealthChecker(HealthChecker&&) = delete;
    HealthChecker& operator=(HealthChecker&&) = delete;

    void Start();
    void Stop();

    void SetProbeCallback(std::function<void(const std::string&, HealthCheckResult)> cb) {
        probe_callback_ = std::move(cb);
    }

    void SetOnBackendUnhealthyCallback(std::function<void(uint32_t)> cb) {
        on_unhealthy_callback_ = std::move(cb);
    }

    [[nodiscard]] HealthCheckType GetType() const noexcept { return check_type_; }
    [[nodiscard]] const std::string& GetHttpPath() const noexcept { return http_path_; }
    [[nodiscard]] uint16_t GetExpectedStatus() const noexcept { return expected_status_; }

private:
    void ScheduleInterval();
    void OnIntervalComplete(const CompletionEvent& event);

    void StartCheck();
    void OnConnectComplete(const CompletionEvent& event);
    void OnWriteComplete(const CompletionEvent& event);
    void OnReadComplete(const CompletionEvent& event);
    void OnTimeoutComplete(const CompletionEvent& event);

    void CompleteCheck(HealthCheckResult result);
    void CancelTimeout();
    void CleanupSocket();

    RingEngine& engine_;
    std::shared_ptr<Backend> backend_;
    std::chrono::milliseconds interval_;
    std::chrono::milliseconds timeout_;
    HealthCheckType check_type_{HealthCheckType::Tcp};
    std::string http_path_{"/health"};
    uint16_t expected_status_{200};

    bool running_{false};
    bool check_in_progress_{false};
    bool connect_finished_{false};
    bool write_finished_{false};
    bool read_finished_{false};
    int socket_fd_{-1};

    std::string http_request_str_;
    size_t http_write_offset_{0};
    char http_response_buf_[1024];
    size_t http_response_len_{0};

    std::function<void(const std::string&, HealthCheckResult)> probe_callback_;
    std::function<void(uint32_t)> on_unhealthy_callback_;

    OpContext interval_ctx_;
    OpContext connect_ctx_;
    OpContext write_ctx_;
    OpContext read_ctx_;
    OpContext timeout_ctx_;
    OpContext cancel_interval_ctx_;
    OpContext cancel_connect_ctx_;
    OpContext cancel_write_ctx_;
    OpContext cancel_read_ctx_;
    OpContext cancel_timeout_ctx_;

    struct __kernel_timespec interval_ts_{};
    struct __kernel_timespec timeout_ts_{};
};

} // namespace helios
