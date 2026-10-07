#pragma once

#include "io/ring_engine.hpp"
#include "io/op_context.hpp"
#include "lb/backend.hpp"
#include <memory>
#include <chrono>

namespace helios {

class HealthChecker {
public:
    HealthChecker(RingEngine& engine, std::shared_ptr<Backend> backend,
                  std::chrono::milliseconds interval = std::chrono::milliseconds(5000),
                  std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));
    ~HealthChecker();

    // Prevent copy and move to ensure stable addresses for context pointers
    HealthChecker(const HealthChecker&) = delete;
    HealthChecker& operator=(const HealthChecker&) = delete;
    HealthChecker(HealthChecker&&) = delete;
    HealthChecker& operator=(HealthChecker&&) = delete;

    void Start();
    void Stop();

private:
    void ScheduleInterval();
    void OnIntervalComplete(const CompletionEvent& event);

    void StartCheck();
    void OnConnectComplete(const CompletionEvent& event);
    void OnTimeoutComplete(const CompletionEvent& event);

    void CompleteCheck(HealthCheckResult result);
    void CleanupSocket();

    RingEngine& engine_;
    std::shared_ptr<Backend> backend_;
    std::chrono::milliseconds interval_;
    std::chrono::milliseconds timeout_;

    bool running_{false};
    bool check_in_progress_{false};
    bool connect_finished_{false};
    int socket_fd_{-1};

    OpContext interval_ctx_;
    OpContext connect_ctx_;
    OpContext timeout_ctx_;
    OpContext cancel_connect_ctx_;

    struct __kernel_timespec interval_ts_{};
    struct __kernel_timespec timeout_ts_{};
};

} // namespace helios
