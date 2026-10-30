#include "lb/health_checker.hpp"
#include "net/socket_utils.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <cerrno>
#include <cstring>

namespace helios {

HealthChecker::HealthChecker(RingEngine& engine, std::shared_ptr<Backend> backend,
                             std::chrono::milliseconds interval,
                             std::chrono::milliseconds timeout,
                             HealthCheckType check_type,
                             std::string http_path,
                             uint16_t expected_status)
    : engine_(engine), backend_(std::move(backend)), interval_(interval), timeout_(timeout),
      check_type_(check_type), http_path_(std::move(http_path)), expected_status_(expected_status) {

    interval_ctx_.on_complete = [this](const CompletionEvent& e) { OnIntervalComplete(e); };
    connect_ctx_.on_complete = [this](const CompletionEvent& e) { OnConnectComplete(e); };
    write_ctx_.on_complete = [this](const CompletionEvent& e) { OnWriteComplete(e); };
    read_ctx_.on_complete = [this](const CompletionEvent& e) { OnReadComplete(e); };
    timeout_ctx_.on_complete = [this](const CompletionEvent& e) { OnTimeoutComplete(e); };

    cancel_interval_ctx_.on_complete = [](const CompletionEvent&) {};
    cancel_connect_ctx_.on_complete = [](const CompletionEvent&) {};
    cancel_write_ctx_.on_complete = [](const CompletionEvent&) {};
    cancel_read_ctx_.on_complete = [](const CompletionEvent&) {};
    cancel_timeout_ctx_.on_complete = [](const CompletionEvent&) {};
}

HealthChecker::~HealthChecker() {
    Stop();

    while (interval_ctx_.in_flight || connect_ctx_.in_flight || write_ctx_.in_flight || read_ctx_.in_flight ||
           timeout_ctx_.in_flight || cancel_interval_ctx_.in_flight || cancel_connect_ctx_.in_flight ||
           cancel_write_ctx_.in_flight || cancel_read_ctx_.in_flight || cancel_timeout_ctx_.in_flight) {
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

    if (interval_ctx_.in_flight && !cancel_interval_ctx_.in_flight) {
        if (engine_.PrepCancel(&interval_ctx_, &cancel_interval_ctx_)) {
            cancel_interval_ctx_.MarkInFlight();
        }
    }
    if (connect_ctx_.in_flight && !cancel_connect_ctx_.in_flight) {
        if (engine_.PrepCancel(&connect_ctx_, &cancel_connect_ctx_)) {
            cancel_connect_ctx_.MarkInFlight();
        }
    }
    if (write_ctx_.in_flight && !cancel_write_ctx_.in_flight) {
        if (engine_.PrepCancel(&write_ctx_, &cancel_write_ctx_)) {
            cancel_write_ctx_.MarkInFlight();
        }
    }
    if (read_ctx_.in_flight && !cancel_read_ctx_.in_flight) {
        if (engine_.PrepCancel(&read_ctx_, &cancel_read_ctx_)) {
            cancel_read_ctx_.MarkInFlight();
        }
    }
    if (timeout_ctx_.in_flight && !cancel_timeout_ctx_.in_flight) {
        if (engine_.PrepCancel(&timeout_ctx_, &cancel_timeout_ctx_)) {
            cancel_timeout_ctx_.MarkInFlight();
        }
    }
    engine_.Submit();

    CleanupSocket();
}

void HealthChecker::ScheduleInterval() {
    if (!running_) return;

    interval_ts_.tv_sec = interval_.count() / 1000;
    interval_ts_.tv_nsec = (interval_.count() % 1000) * 1000000;

    if (engine_.PrepTimeout(&interval_ts_, &interval_ctx_)) {
        interval_ctx_.MarkInFlight();
        engine_.Submit();
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
    write_finished_ = false;
    read_finished_ = false;

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
        engine_.Submit();
    } else {
        CleanupSocket();
        CompleteCheck(HealthCheckResult::ConnectionFailure);
    }
}

void HealthChecker::CancelTimeout() {
    if (timeout_ctx_.in_flight && !cancel_timeout_ctx_.in_flight) {
        if (engine_.PrepCancel(&timeout_ctx_, &cancel_timeout_ctx_)) {
            cancel_timeout_ctx_.MarkInFlight();
            engine_.Submit();
        }
    }
}

void HealthChecker::OnConnectComplete(const CompletionEvent& event) {
    if (!running_ || !check_in_progress_ || connect_finished_ || event.result == -ECANCELED) return;
    connect_finished_ = true;

    if (!event.IsSuccess()) {
        CancelTimeout();
        CompleteCheck(HealthCheckResult::ConnectionFailure);
        return;
    }

    if (check_type_ == HealthCheckType::Tcp) {
        CancelTimeout();
        CompleteCheck(HealthCheckResult::Success);
        return;
    }

    // HTTP probe: Send HTTP GET request
    http_request_str_ = "GET " + http_path_ + " HTTP/1.1\r\n"
                        "Host: " + backend_->GetHost() + ":" + std::to_string(backend_->GetPort()) + "\r\n"
                        "User-Agent: helios-healthcheck/1.0\r\n"
                        "Accept: */*\r\n"
                        "Connection: close\r\n\r\n";
    http_write_offset_ = 0;

    if (engine_.PrepWrite(socket_fd_, http_request_str_.data(), http_request_str_.size(), 0, &write_ctx_)) {
        write_ctx_.MarkInFlight();
        engine_.Submit();
    } else {
        CancelTimeout();
        CompleteCheck(HealthCheckResult::ConnectionFailure);
    }
}

void HealthChecker::OnWriteComplete(const CompletionEvent& event) {
    if (!running_ || !check_in_progress_ || write_finished_ || event.result == -ECANCELED) return;

    if (event.result <= 0) {
        write_finished_ = true;
        CancelTimeout();
        CompleteCheck(HealthCheckResult::ConnectionFailure);
        return;
    }

    http_write_offset_ += static_cast<size_t>(event.result);
    if (http_write_offset_ < http_request_str_.size()) {
        if (engine_.PrepWrite(socket_fd_, http_request_str_.data() + http_write_offset_,
                              http_request_str_.size() - http_write_offset_, 0, &write_ctx_)) {
            write_ctx_.MarkInFlight();
            engine_.Submit();
            return;
        } else {
            write_finished_ = true;
            CancelTimeout();
            CompleteCheck(HealthCheckResult::ConnectionFailure);
            return;
        }
    }

    write_finished_ = true;

    // Request written; prepare to read HTTP response status line
    std::memset(http_response_buf_, 0, sizeof(http_response_buf_));
    http_response_len_ = 0;

    if (engine_.PrepRead(socket_fd_, http_response_buf_, sizeof(http_response_buf_) - 1, 0, &read_ctx_)) {
        read_ctx_.MarkInFlight();
        engine_.Submit();
    } else {
        CancelTimeout();
        CompleteCheck(HealthCheckResult::ConnectionFailure);
    }
}

void HealthChecker::OnReadComplete(const CompletionEvent& event) {
    if (!running_ || !check_in_progress_ || read_finished_ || event.result == -ECANCELED) return;
    read_finished_ = true;

    CancelTimeout();

    if (event.result <= 0) {
        CompleteCheck(HealthCheckResult::ConnectionFailure);
        return;
    }

    http_response_len_ = static_cast<size_t>(event.result);
    http_response_buf_[http_response_len_] = '\0';

    std::string_view resp(http_response_buf_, http_response_len_);
    if (resp.rfind("HTTP/", 0) != 0) {
        CompleteCheck(HealthCheckResult::ProtocolFailure);
        return;
    }

    auto sp1 = resp.find(' ');
    if (sp1 == std::string_view::npos || sp1 + 4 > resp.size()) {
        CompleteCheck(HealthCheckResult::ProtocolFailure);
        return;
    }

    int code = 0;
    for (size_t i = sp1 + 1; i < sp1 + 4 && i < resp.size(); ++i) {
        if (resp[i] >= '0' && resp[i] <= '9') {
            code = code * 10 + (resp[i] - '0');
        } else {
            CompleteCheck(HealthCheckResult::ProtocolFailure);
            return;
        }
    }

    bool success = false;
    if (expected_status_ == 0) {
        success = (code >= 200 && code < 300);
    } else {
        success = (code == expected_status_);
    }

    if (success) {
        CompleteCheck(HealthCheckResult::Success);
    } else {
        CompleteCheck(HealthCheckResult::ProtocolFailure);
    }
}

void HealthChecker::OnTimeoutComplete(const CompletionEvent& event) {
    if (!running_ || !check_in_progress_ || event.result == -ECANCELED) return;

    if (connect_ctx_.in_flight && !cancel_connect_ctx_.in_flight) {
        if (engine_.PrepCancel(&connect_ctx_, &cancel_connect_ctx_)) {
            cancel_connect_ctx_.MarkInFlight();
        }
    }
    if (write_ctx_.in_flight && !cancel_write_ctx_.in_flight) {
        if (engine_.PrepCancel(&write_ctx_, &cancel_write_ctx_)) {
            cancel_write_ctx_.MarkInFlight();
        }
    }
    if (read_ctx_.in_flight && !cancel_read_ctx_.in_flight) {
        if (engine_.PrepCancel(&read_ctx_, &cancel_read_ctx_)) {
            cancel_read_ctx_.MarkInFlight();
        }
    }
    engine_.Submit();

    CompleteCheck(HealthCheckResult::Timeout);
}

void HealthChecker::CompleteCheck(HealthCheckResult result) {
    if (!check_in_progress_) return;
    check_in_progress_ = false;

    HealthState prev_state = backend_->GetHealthState();
    backend_->ReportHealthCheckResult(result);
    HealthState new_state = backend_->GetHealthState();

    if (prev_state != HealthState::Unhealthy && new_state == HealthState::Unhealthy) {
        if (on_unhealthy_callback_) {
            on_unhealthy_callback_(backend_->GetId());
        }
    }

    if (probe_callback_) {
        std::string ep = backend_->GetHost() + ":" + std::to_string(backend_->GetPort());
        probe_callback_(ep, result);
    }

    CleanupSocket();
    ScheduleInterval();
}

void HealthChecker::CleanupSocket() {
    if (socket_fd_ != -1) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

} // namespace helios
