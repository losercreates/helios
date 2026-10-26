#include "worker.hpp"
#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <future>
#include <algorithm>

namespace helios {

namespace {

WorkerConfig MakeWorkerConfigWithReusePort(WorkerConfig config) {
    config.proxy_config.reuse_port = true;
    return config;
}

} // namespace

Worker::Worker(uint32_t worker_id, int core_id, WorkerConfig config)
    : worker_id_(worker_id),
      core_id_(core_id),
      config_(MakeWorkerConfigWithReusePort(std::move(config))),
      proxy_(config_.proxy_config) {
    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

Worker::~Worker() {
    Stop();
    if (event_fd_ >= 0) {
        ::close(event_fd_);
        event_fd_ = -1;
    }
}

bool Worker::PinToCore(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    size_t num_cores = std::thread::hardware_concurrency();
    if (num_cores == 0) num_cores = 1;
    size_t target_core = (core_id >= 0) ? (static_cast<size_t>(core_id) % num_cores) : 0;
    CPU_SET(target_core, &cpuset);

    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc == 0) {
        affinity_set_.store(true, std::memory_order_release);
        return true;
    }
    return false;
}

bool Worker::CheckAffinity(int expected_core) const {
    if (!thread_.joinable()) return false;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    int rc = pthread_getaffinity_np(const_cast<std::thread&>(thread_).native_handle(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) return false;
    size_t num_cores = std::thread::hardware_concurrency();
    if (num_cores == 0) num_cores = 1;
    size_t target_core = (expected_core >= 0) ? (static_cast<size_t>(expected_core) % num_cores) : 0;
    return CPU_ISSET(target_core, &cpuset);
}

bool Worker::Start() {
    if (running_.load(std::memory_order_acquire)) return true;

    stop_requested_.store(false, std::memory_order_release);
    affinity_set_.store(false, std::memory_order_release);
    listener_closed_.store(false, std::memory_order_release);

    std::promise<bool> start_promise;
    auto start_future = start_promise.get_future();

    thread_ = std::thread([this, &start_promise]() {
        PinToCore(core_id_);

        if (!proxy_.Start()) {
            start_promise.set_value(false);
            return;
        }

        running_.store(true, std::memory_order_release);
        start_promise.set_value(true);

        Run();
    });

    bool success = start_future.get();
    if (!success) {
        if (thread_.joinable()) thread_.join();
        return false;
    }
    return true;
}

void Worker::Run() {
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        if (event_fd_ >= 0) {
            uint64_t val = 0;
            ssize_t n = ::read(event_fd_, &val, sizeof(val));
            if (n > 0 && (val & 1)) {
                stop_requested_.store(true, std::memory_order_relaxed);
                break;
            }
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

        proxy_.RunOnce(std::chrono::milliseconds(1));
    }

    // Coordinated Graceful Draining:
    // 1. Cease accepting new connections by closing listener and cancelling in-flight accept
    proxy_.CloseListener();
    proxy_.RunOnce(std::chrono::milliseconds(1));
    listener_closed_.store(true, std::memory_order_release);

    // 2. Allow active connections up to drain_timeout to complete
    auto drain_start = std::chrono::steady_clock::now();
    while (proxy_.GetActiveConnectionCount() > 0) {
        proxy_.RunOnce(std::chrono::milliseconds(5));
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - drain_start);
        if (elapsed >= config_.drain_timeout) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    proxy_.Stop();
    running_.store(false, std::memory_order_release);
}

void Worker::NotifyStop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    if (event_fd_ >= 0) {
        uint64_t val = 1;
        ssize_t ret = ::write(event_fd_, &val, sizeof(val));
        (void)ret;
    }
}

void Worker::Stop() {
    NotifyStop();
    Join();
}

void Worker::Join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Worker::AddBackend(std::string host, uint16_t port, uint32_t weight) {
    proxy_.AddBackend(std::move(host), port, weight);
}

void Worker::SetListenPort(uint16_t port) noexcept {
    config_.proxy_config.listen_port = port;
    proxy_.SetListenPort(port);
}

void Worker::Post(std::function<void()> task) {
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

// ==========================================
// WorkerPool Implementation
// ==========================================

WorkerPool::WorkerPool(WorkerPoolConfig config)
    : config_(std::move(config)) {}

WorkerPool::~WorkerPool() {
    Stop();
}

void WorkerPool::AddBackend(std::string host, uint16_t port, uint32_t weight) {
    initial_backends_.emplace_back(host, port, weight);
    for (auto& w : workers_) {
        w->AddBackend(host, port, weight);
    }
}

void WorkerPool::SetAccessLogger(std::shared_ptr<AccessLogger> logger) {
    access_logger_ = std::move(logger);
    for (auto& w : workers_) {
        w->SetAccessLogger(access_logger_);
    }
}

void WorkerPool::SetMetricsCollector(std::shared_ptr<MetricsCollector> collector) {
    metrics_collector_ = std::move(collector);
    for (size_t i = 0; i < workers_.size(); ++i) {
        if (metrics_collector_) {
            workers_[i]->SetMetrics(metrics_collector_->GetOrCreateWorkerMetrics(static_cast<uint32_t>(i)));
        }
    }
}

void WorkerPool::ReloadBackends(const std::vector<BackendInfo>& new_backends) {
    initial_backends_.clear();
    for (const auto& b : new_backends) {
        initial_backends_.emplace_back(b.host, b.port, b.weight);
    }

    if (workers_.empty()) return;

    std::vector<std::future<void>> futures;
    futures.reserve(workers_.size());

    for (auto& worker : workers_) {
        auto promise = std::make_shared<std::promise<void>>();
        futures.push_back(promise->get_future());

        worker->Post([w = worker.get(), endpoints = new_backends, p = promise]() {
            auto& lb = w->GetProxy().GetLoadBalancer();
            auto existing = lb.GetAllBackends();

            // 1. Identify backends to remove
            for (const auto& b : existing) {
                if (!b) continue;
                bool found = false;
                for (const auto& ep : endpoints) {
                    if (b->GetHost() == ep.host && b->GetPort() == ep.port) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    b->SetDraining(true);
                    lb.RemoveBackend(b->GetId());
                }
            }

            // 2. Identify backends to add or update weight
            for (const auto& ep : endpoints) {
                bool found = false;
                for (const auto& b : existing) {
                    if (b && b->GetHost() == ep.host && b->GetPort() == ep.port) {
                        found = true;
                        if (b->GetWeight() != ep.weight) {
                            b->SetWeight(ep.weight);
                        }
                        break;
                    }
                }
                if (!found) {
                    w->AddBackend(ep.host, ep.port, ep.weight);
                }
            }

            p->set_value();
        });
    }

    for (auto& f : futures) {
        if (f.valid()) {
            f.wait_for(std::chrono::seconds(2));
        }
    }
}

void WorkerPool::SetAcl(std::shared_ptr<IpAcl> acl) {
    acl_ = std::move(acl);
    for (auto& w : workers_) {
        w->GetProxy().SetAcl(acl_);
    }
}

void WorkerPool::SetRateLimiter(std::shared_ptr<RateLimiter> limiter) {
    rate_limiter_ = std::move(limiter);
    for (auto& w : workers_) {
        w->GetProxy().SetRateLimiter(rate_limiter_);
    }
}

void WorkerPool::SetTlsContext(std::shared_ptr<TlsContext> ctx) {
    tls_context_ = std::move(ctx);
    for (auto& w : workers_) {
        w->GetProxy().SetTlsContext(tls_context_);
    }
}

void WorkerPool::ReloadSecurity(const SecurityConfig& security) {
    config_.security = security;
    if (security.acl.enabled) {
        AclAction default_policy = (security.acl.default_policy == "deny") ? AclAction::Deny : AclAction::Allow;
        auto new_acl = std::make_shared<IpAcl>(default_policy, true);
        for (const auto& r : security.acl.rules) {
            AclAction action = (r.action == "deny") ? AclAction::Deny : AclAction::Allow;
            auto parsed = AclRule::Parse(r.cidr, action);
            if (parsed) {
                new_acl->AddRule(std::move(*parsed));
            }
        }
        SetAcl(std::move(new_acl));
    } else {
        SetAcl(nullptr);
    }

    if (security.rate_limit.enabled) {
        if (!rate_limiter_) {
            SetRateLimiter(std::make_shared<RateLimiter>(security.rate_limit));
        } else {
            rate_limiter_->Configure(security.rate_limit.rate_per_sec, security.rate_limit.burst_size, security.rate_limit.cidr_mask);
            rate_limiter_->SetEnabled(true);
        }
    } else if (rate_limiter_) {
        rate_limiter_->SetEnabled(false);
    }
}

bool WorkerPool::ReloadTls(const TlsConfig& tls_config, std::string* error_out) {
    if (!tls_config.enabled) {
        tls_context_ = nullptr;
        for (auto& w : workers_) {
            w->GetProxy().SetTlsContext(nullptr);
        }
        config_.tls = tls_config;
        return true;
    }

    if (!tls_context_) {
        auto new_ctx = std::make_shared<TlsContext>(tls_config);
        if (!new_ctx->Initialize(error_out)) {
            return false;
        }
        tls_context_ = std::move(new_ctx);
    } else {
        if (!tls_context_->Reload(tls_config, error_out)) {
            return false;
        }
    }

    config_.tls = tls_config;
    for (auto& w : workers_) {
        w->GetProxy().SetTlsContext(tls_context_);
    }
    return true;
}

bool WorkerPool::Start() {
    if (running_.load(std::memory_order_acquire)) return true;

    size_t count = config_.num_workers;
    if (count == 0) {
        count = std::thread::hardware_concurrency();
        if (count == 0) count = 2;
    }

    workers_.clear();
    workers_.reserve(count);

    if (!acl_ && config_.security.acl.enabled) {
        AclAction default_policy = (config_.security.acl.default_policy == "deny") ? AclAction::Deny : AclAction::Allow;
        acl_ = std::make_shared<IpAcl>(default_policy, true);
        for (const auto& r : config_.security.acl.rules) {
            AclAction action = (r.action == "deny") ? AclAction::Deny : AclAction::Allow;
            auto parsed = AclRule::Parse(r.cidr, action);
            if (parsed) {
                acl_->AddRule(std::move(*parsed));
            }
        }
    }

    if (!rate_limiter_ && config_.security.rate_limit.enabled) {
        rate_limiter_ = std::make_shared<RateLimiter>(config_.security.rate_limit);
    }

    if (config_.tls.enabled && !tls_context_) {
        tls_context_ = std::make_shared<TlsContext>(config_.tls);
        std::string err;
        if (!tls_context_->Initialize(&err)) {
            return false;
        }
    }

    uint16_t target_port = config_.listen_port;

    for (size_t i = 0; i < count; ++i) {
        WorkerConfig wcfg;
        wcfg.worker_id = static_cast<uint32_t>(i);
        wcfg.core_id = static_cast<int>(i);
        wcfg.drain_timeout = config_.drain_timeout;
        wcfg.proxy_config.listen_host = config_.listen_host;
        wcfg.proxy_config.listen_port = target_port;
        wcfg.proxy_config.reuse_port = true;
        wcfg.proxy_config.lb_algorithm = config_.lb_algorithm;
        wcfg.proxy_config.ring_entries = config_.ring_entries;
        wcfg.proxy_config.total_buffers = config_.total_buffers;
        wcfg.proxy_config.buffer_size = config_.buffer_size;
        wcfg.proxy_config.enable_connection_pooling = config_.enable_connection_pooling;
        wcfg.proxy_config.connection_pool_config = config_.connection_pool_config;
        wcfg.proxy_config.header_read_timeout = config_.header_read_timeout;
        wcfg.proxy_config.enable_tls = config_.tls.enabled;
        wcfg.proxy_config.enable_l7_routing = config_.tls.enabled;
        wcfg.proxy_config.tls_config = config_.tls;

        auto worker = std::make_unique<Worker>(wcfg.worker_id, wcfg.core_id, std::move(wcfg));
        if (access_logger_) {
            worker->SetAccessLogger(access_logger_);
        }
        if (metrics_collector_) {
            worker->SetMetrics(metrics_collector_->GetOrCreateWorkerMetrics(wcfg.worker_id));
        }
        if (acl_) {
            worker->GetProxy().SetAcl(acl_);
        }
        if (rate_limiter_) {
            worker->GetProxy().SetRateLimiter(rate_limiter_);
        }
        if (tls_context_) {
            worker->GetProxy().SetTlsContext(tls_context_);
        }
        for (const auto& [host, port, weight] : initial_backends_) {
            worker->AddBackend(host, port, weight);
        }
        if (config_.tls.enabled && worker->GetProxy().GetRouter().GetDefaultPool().empty()) {
            worker->GetProxy().SetDefaultPool("default");
        }

        if (i == 0) {
            if (!worker->Start()) {
                return false;
            }
            actual_port_ = worker->GetPort();
            target_port = actual_port_;
        } else {
            worker->SetListenPort(target_port);
            if (!worker->Start()) {
                Stop();
                return false;
            }
        }

        workers_.push_back(std::move(worker));
    }

    running_.store(true, std::memory_order_release);
    return true;
}

void WorkerPool::NotifyStop() noexcept {
    for (auto& w : workers_) {
        w->NotifyStop();
    }
}

void WorkerPool::Stop() {
    NotifyStop();
    Join();
    running_.store(false, std::memory_order_release);
}

void WorkerPool::Join() {
    for (auto& w : workers_) {
        w->Join();
    }
}

size_t WorkerPool::GetTotalActiveConnections() const noexcept {
    size_t total = 0;
    for (const auto& w : workers_) {
        total += w->GetActiveConnections();
    }
    return total;
}

uint64_t WorkerPool::GetTotalAcceptedConnections() const noexcept {
    uint64_t total = 0;
    for (const auto& w : workers_) {
        total += w->GetAcceptedConnections();
    }
    return total;
}

bool WorkerPool::AreAllListenersClosed() const noexcept {
    for (const auto& w : workers_) {
        if (w && !w->IsListenerClosed()) {
            return false;
        }
    }
    return true;
}

bool WorkerPool::IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

} // namespace helios
