#include "lb/health_checker.hpp"
#include "net/socket_utils.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <cerrno>

namespace helios {

HealthChecker::HealthChecker(RingEngine& engine, std::shared_ptr<Backend> backend,
                             std::chrono::milliseconds interval,
                             std::chrono::milliseconds timeout)
    : engine_(engine), backend_(std::move(backend)), interval_(interval), timeout_(timeout) {

    interval_ctx_.on_complete = [this](const CompletionEvent& e) { OnIntervalComplete(e); };
    connect_ctx_.on_complete = [this](const CompletionEvent& e) { OnConnectComplete(e); };
    timeout_ctx_.on_complete = [this](const CompletionEvent& e) { OnTimeoutComplete(e); };
}

HealthChecker::~HealthChecker() {
    Stop();

    auto cancel_if_in_flight = [this](OpContext& ctx) {
        if (ctx.in_flight) {
            auto cancel_ctx = new OpContext();
            cancel_ctx->on_complete = [cancel_ctx](const CompletionEvent&) { delete cancel_ctx; };
            engine_.PrepCancel(&ctx, cancel_ctx);
        }
    };
    cancel_if_in_flight(interval_ctx_);
    cancel_if_in_flight(connect_ctx_);
    cancel_if_in_flight(timeout_ctx_);
    engine_.Submit();

    while (interval_ctx_.in_flight || connect_ctx_.in_flight || timeout_ctx_.in_flight || cancel_connect_ctx_.in_flight) {
        engine_.SubmitAndWait(1);
        engine_.ProcessCompletions();
    }
}

void HealthChecker::Start() {
    if (running_) return;
    running_ = true;
    StartCheck();
}

void HealthChecker::Stop() {
    if (!running_) return;
    running_ = false;

    CleanupSocket();
    
    // Cancellations are handled in the destructor or by the user.
}

void HealthChecker::ScheduleInterval() {
    if (!running_) return;
    
    interval_ts_.tv_sec = interval_.count() / 1000;
    interval_ts_.tv_nsec = (interval_.count() % 1000) * 1000000;
    
    if (engine_.PrepTimeout(&interval_ts_, &interval_ctx_)) {
        interval_ctx_.MarkInFlight();
    }
}

void HealthChecker::OnIntervalComplete(const CompletionEvent& event) {
    if (!running_) return;
    
    if (event.result == -ECANCELED) return;
    
    StartCheck();
}

void HealthChecker::StartCheck() {
    if (!running_ || check_in_progress_) return;
    
    check_in_progress_ = true;
    connect_finished_ = false;
    
    socket_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) {
        CompleteCheck(HealthCheckResult::ConnectionFailure);
        return;
    }
    
    SocketUtils::SetNonBlocking(socket_fd_);
    
    if (engine_.PrepConnect(socket_fd_, reinterpret_cast<const sockaddr*>(&backend_->GetSockAddr()), sizeof(sockaddr_in), &connect_ctx_)) {
        connect_ctx_.MarkInFlight();
        
        timeout_ts_.tv_sec = timeout_.count() / 1000;
        timeout_ts_.tv_nsec = (timeout_.count() % 1000) * 1000000;
        
        if (engine_.PrepTimeout(&timeout_ts_, &timeout_ctx_)) {
            timeout_ctx_.MarkInFlight();
        }
    } else {
        CleanupSocket();
        CompleteCheck(HealthCheckResult::ConnectionFailure);
    }
}

void HealthChecker::OnConnectComplete(const CompletionEvent& event) {
    if (connect_finished_) return;
    connect_finished_ = true;
    
    if (timeout_ctx_.in_flight) {
        engine_.PrepCancel(&timeout_ctx_, &cancel_connect_ctx_);
    }
    
    if (event.IsSuccess()) {
        CompleteCheck(HealthCheckResult::Success);
    } else {
        CompleteCheck(HealthCheckResult::ConnectionFailure);
    }
}

void HealthChecker::OnTimeoutComplete(const CompletionEvent& event) {
    if (connect_finished_ || event.result == -ECANCELED) return;
    connect_finished_ = true;
    
    if (connect_ctx_.in_flight) {
        engine_.PrepCancel(&connect_ctx_, &cancel_connect_ctx_);
    }
    
    CompleteCheck(HealthCheckResult::Timeout);
}

void HealthChecker::CompleteCheck(HealthCheckResult result) {
    backend_->ReportHealthCheckResult(result);
    CleanupSocket();
    check_in_progress_ = false;
    ScheduleInterval();
}

void HealthChecker::CleanupSocket() {
    if (socket_fd_ != -1) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

} // namespace helios
