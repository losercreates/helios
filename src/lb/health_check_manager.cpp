#include "lb/health_check_manager.hpp"
#include <sys/eventfd.h>
#include <unistd.h>
#include <poll.h>
#include <future>
#include <unordered_set>
#include <algorithm>

namespace helios {

HealthCheckManager::HealthCheckManager(HealthCheckConfig config)
    : config_(std::move(config)), engine_(128) {
    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

HealthCheckManager::~HealthCheckManager() {
    Stop();
    if (event_fd_ >= 0) {
        ::close(event_fd_);
        event_fd_ = -1;
    }
}

bool HealthCheckManager::Start() {
    if (running_.load(std::memory_order_acquire)) return true;

    stop_requested_.store(false, std::memory_order_release);

    std::promise<bool> start_promise;
    auto start_future = start_promise.get_future();

    thread_ = std::thread([this, &start_promise]() {
        running_.store(true, std::memory_order_release);
        start_promise.set_value(true);
        Run();
    });

    return start_future.get();
}

void HealthCheckManager::Stop() {
    if (!running_.load(std::memory_order_acquire)) return;

    stop_requested_.store(true, std::memory_order_release);
    if (event_fd_ >= 0) {
        uint64_t val = 1;
        ssize_t ret = ::write(event_fd_, &val, sizeof(val));
        (void)ret;
    }

    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_release);
}

void HealthCheckManager::PostTask(std::function<void()> task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lock(task_mutex_);
        pending_tasks_.push_back(std::move(task));
    }
    has_tasks_.store(true, std::memory_order_release);
    if (event_fd_ >= 0) {
        uint64_t val = 2;
        ssize_t ret = ::write(event_fd_, &val, sizeof(val));
        (void)ret;
    }
}

std::unique_ptr<HealthChecker> HealthCheckManager::CreateChecker(std::shared_ptr<Backend> backend) {
    auto checker = std::make_unique<HealthChecker>(
        engine_, backend, config_.interval, config_.timeout,
        config_.type == "http" ? HealthCheckType::Http : HealthCheckType::Tcp,
        config_.path, config_.expected_status);

    if (on_backend_unhealthy_callback_) {
        checker->SetOnBackendUnhealthyCallback(on_backend_unhealthy_callback_);
    }

    checker->SetProbeCallback([this](const std::string& endpoint, HealthCheckResult result) {
        if (metrics_collector_) {
            std::string res_str;
            switch (result) {
                case HealthCheckResult::Success: res_str = "success"; break;
                case HealthCheckResult::ConnectionFailure: res_str = "failure"; break;
                case HealthCheckResult::Timeout: res_str = "timeout"; break;
                case HealthCheckResult::ProtocolFailure: res_str = "protocol_error"; break;
            }
            metrics_collector_->RecordHealthCheckProbe(endpoint, res_str);
        }
    });

    return checker;
}

void HealthCheckManager::SetMetricsCollector(std::shared_ptr<MetricsCollector> collector) {
    metrics_collector_ = std::move(collector);
    if (!running_.load(std::memory_order_acquire)) return;
    PostTask([this]() {
        std::lock_guard<std::mutex> lock(checkers_mutex_);
        for (auto& [_, checker] : checkers_) {
            checker->SetProbeCallback([this](const std::string& endpoint, HealthCheckResult result) {
                if (metrics_collector_) {
                    std::string res_str;
                    switch (result) {
                        case HealthCheckResult::Success: res_str = "success"; break;
                        case HealthCheckResult::ConnectionFailure: res_str = "failure"; break;
                        case HealthCheckResult::Timeout: res_str = "timeout"; break;
                        case HealthCheckResult::ProtocolFailure: res_str = "protocol_error"; break;
                    }
                    metrics_collector_->RecordHealthCheckProbe(endpoint, res_str);
                }
            });
        }
    });
}

void HealthCheckManager::SetOnBackendUnhealthyCallback(std::function<void(uint32_t)> cb) {
    on_backend_unhealthy_callback_ = std::move(cb);
    if (!running_.load(std::memory_order_acquire)) return;
    PostTask([this]() {
        std::lock_guard<std::mutex> lock(checkers_mutex_);
        for (auto& [_, checker] : checkers_) {
            checker->SetOnBackendUnhealthyCallback(on_backend_unhealthy_callback_);
        }
    });
}

void HealthCheckManager::RegisterBackend(std::shared_ptr<Backend> backend) {
    if (!backend) return;
    if (!running_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(checkers_mutex_);
        registered_backends_.push_back(std::move(backend));
        return;
    }
    PostTask([this, b = std::move(backend)]() {
        std::lock_guard<std::mutex> lock(checkers_mutex_);
        registered_backends_.push_back(b);
        if (checkers_.find(b->GetId()) == checkers_.end()) {
            b->SetHealthThresholds(config_.unhealthy_threshold, config_.healthy_threshold);
            auto checker = CreateChecker(b);
            checker->Start();
            checkers_.emplace(b->GetId(), std::move(checker));
        }
    });
}

void HealthCheckManager::UnregisterBackend(uint32_t backend_id) {
    if (!running_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(checkers_mutex_);
        registered_backends_.erase(
            std::remove_if(registered_backends_.begin(), registered_backends_.end(),
                           [backend_id](const auto& b) { return b && b->GetId() == backend_id; }),
            registered_backends_.end());
        return;
    }
    PostTask([this, backend_id]() {
        std::lock_guard<std::mutex> lock(checkers_mutex_);
        registered_backends_.erase(
            std::remove_if(registered_backends_.begin(), registered_backends_.end(),
                           [backend_id](const auto& b) { return b && b->GetId() == backend_id; }),
            registered_backends_.end());
        auto it = checkers_.find(backend_id);
        if (it != checkers_.end()) {
            it->second->Stop();
            checkers_.erase(it);
        }
    });
}

void HealthCheckManager::UpdateBackends(const std::vector<std::shared_ptr<Backend>>& backends) {
    if (!running_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(checkers_mutex_);
        registered_backends_ = backends;
        return;
    }
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    PostTask([this, backends, promise]() {
        std::lock_guard<std::mutex> lock(checkers_mutex_);
        registered_backends_ = backends;
        std::unordered_set<uint32_t> new_ids;
        for (const auto& b : backends) {
            if (b) new_ids.insert(b->GetId());
        }
        for (auto it = checkers_.begin(); it != checkers_.end();) {
            if (new_ids.find(it->first) == new_ids.end()) {
                it->second->Stop();
                it = checkers_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& b : backends) {
            if (!b) continue;
            if (checkers_.find(b->GetId()) == checkers_.end()) {
                b->SetHealthThresholds(config_.unhealthy_threshold, config_.healthy_threshold);
                auto checker = CreateChecker(b);
                checker->Start();
                checkers_.emplace(b->GetId(), std::move(checker));
            }
        }
        promise->set_value();
    });
    if (future.valid()) {
        future.wait_for(std::chrono::seconds(2));
    }
}

void HealthCheckManager::SetConfig(HealthCheckConfig config) {
    PostTask([this, cfg = std::move(config)]() {
        config_ = cfg;
        std::lock_guard<std::mutex> lock(checkers_mutex_);
        for (const auto& b : registered_backends_) {
            if (!b) continue;
            b->SetHealthThresholds(config_.unhealthy_threshold, config_.healthy_threshold);
            auto it = checkers_.find(b->GetId());
            if (it != checkers_.end()) {
                it->second->Stop();
                it->second = CreateChecker(b);
                it->second->Start();
            }
        }
    });
}

size_t HealthCheckManager::GetActiveCheckerCount() const {
    std::lock_guard<std::mutex> lock(checkers_mutex_);
    return checkers_.size();
}

void HealthCheckManager::Run() {
    OpContext wake_ctx;
    wake_ctx.on_complete = [](const CompletionEvent&) {};
    bool wake_in_flight = false;

    // Initialize checkers for registered backends
    {
        std::lock_guard<std::mutex> lock(checkers_mutex_);
        for (auto& b : registered_backends_) {
            if (!b) continue;
            b->SetHealthThresholds(config_.unhealthy_threshold, config_.healthy_threshold);
            auto checker = CreateChecker(b);
            checker->Start();
            checkers_.emplace(b->GetId(), std::move(checker));
        }
    }

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        if (!wake_in_flight && event_fd_ >= 0) {
            if (engine_.PrepPoll(event_fd_, POLLIN, &wake_ctx)) {
                wake_ctx.MarkInFlight();
                wake_in_flight = true;
            }
        }

        engine_.Submit();

        if (engine_.InFlightOps() > 0) {
            engine_.SubmitAndWait(1);
        }

        engine_.ProcessCompletions();

        if (!wake_ctx.in_flight && wake_in_flight) {
            wake_in_flight = false;
            uint64_t val = 0;
            ssize_t ret = ::read(event_fd_, &val, sizeof(val));
            (void)ret;
        }

        if (has_tasks_.load(std::memory_order_acquire)) {
            std::vector<std::function<void()>> tasks;
            {
                std::lock_guard<std::mutex> lock(task_mutex_);
                tasks.swap(pending_tasks_);
                has_tasks_.store(false, std::memory_order_release);
            }
            for (auto& task : tasks) {
                if (task) task();
            }
        }
    }

    // Stop and destroy all active checkers cleanly on this thread
    {
        std::lock_guard<std::mutex> lock(checkers_mutex_);
        for (auto& [_, checker] : checkers_) {
            checker->Stop();
        }
        checkers_.clear();
    }

    // Cancel and drain wake_ctx if still in flight
    if (wake_ctx.in_flight) {
        OpContext cancel_wake;
        cancel_wake.on_complete = [](const CompletionEvent&) {};
        if (engine_.PrepCancel(&wake_ctx, &cancel_wake)) {
            cancel_wake.MarkInFlight();
            engine_.Submit();
        }
        while (wake_ctx.in_flight || cancel_wake.in_flight) {
            engine_.SubmitAndWait(1);
            engine_.ProcessCompletions();
        }
    }

    // Drain any remaining completions
    while (engine_.InFlightOps() > 0) {
        engine_.SubmitAndWait(1);
        engine_.ProcessCompletions();
    }

    engine_.Shutdown();
    running_.store(false, std::memory_order_release);
}

} // namespace helios

