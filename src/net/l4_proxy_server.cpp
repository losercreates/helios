#include "l4_proxy_server.hpp"
#include "socket_utils.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string_view>
#include <array>
#include <vector>
#include <algorithm>
#include <iostream>
#include <cstring>
#include "lb/weighted_round_robin.hpp"
#include "lb/least_connections.hpp"
#include "lb/consistent_hash.hpp"

namespace helios {

L4ProxyServer::L4ProxyServer()
    : L4ProxyServer(Config{}) {}

L4ProxyServer::L4ProxyServer(Config config)
    : config_(std::move(config)),
      engine_(static_cast<uint32_t>(config_.ring_entries)),
      buffer_pool_(config_.total_buffers, config_.buffer_size),
      backpressure_ctrl_(buffer_pool_),
      listener_(engine_),
      timeout_check_interval_(config_.timeout_check_interval) {
    
    if (config_.lb_algorithm == "weighted_round_robin") {
        lb_ = std::make_unique<WeightedRoundRobinLoadBalancer>();
    } else if (config_.lb_algorithm == "least_connections") {
        lb_ = std::make_unique<LeastConnectionsLoadBalancer>();
    } else if (config_.lb_algorithm == "consistent_hash") {
        lb_ = std::make_unique<ConsistentHashLoadBalancer>();
    } else {
        lb_ = std::make_unique<RoundRobinLoadBalancer>();
    }

    if (config_.enable_connection_pooling) {
        backend_pool_ = std::make_unique<BackendConnectionPool>(config_.connection_pool_config);
    }

    if (config_.enable_l7_routing) {
        for (const auto& r : config_.routes) {
            router_.AddRoute(r);
        }
        if (!config_.default_pool.empty()) {
            router_.SetDefaultPool(config_.default_pool);
        }
    }

    accept_ctx_.role = ProxyOpRole::Accept;
}

L4ProxyServer::~L4ProxyServer() {
    Stop();
}

void L4ProxyServer::AddBackend(std::string host, uint16_t port, uint32_t weight) {
    lb_->AddBackend(std::make_shared<Backend>(next_backend_id_++, std::move(host), port, weight));
}

void L4ProxyServer::AddNamedPool(std::string name, std::string algorithm, bool enable_pool, BackendConnectionPoolConfig pool_cfg) {
    NamedPool np;
    np.name = name;
    np.algorithm = algorithm;
    if (algorithm == "weighted_round_robin") {
        np.lb = std::make_unique<WeightedRoundRobinLoadBalancer>();
    } else if (algorithm == "least_connections") {
        np.lb = std::make_unique<LeastConnectionsLoadBalancer>();
    } else if (algorithm == "consistent_hash") {
        np.lb = std::make_unique<ConsistentHashLoadBalancer>();
    } else {
        np.lb = std::make_unique<RoundRobinLoadBalancer>();
    }
    if (enable_pool) {
        np.pool = std::make_unique<BackendConnectionPool>(pool_cfg);
    }
    named_pools_.insert_or_assign(name, std::move(np));
}

void L4ProxyServer::AddBackendToNamedPool(const std::string& pool_name, std::string host, uint16_t port, uint32_t weight) {
    auto it = named_pools_.find(pool_name);
    if (it != named_pools_.end() && it->second.lb) {
        it->second.lb->AddBackend(std::make_shared<Backend>(next_backend_id_++, std::move(host), port, weight));
    }
}

void L4ProxyServer::AddRoute(L7RouteConfig route) {
    config_.enable_l7_routing = true;
    router_.AddRoute(std::move(route));
}

void L4ProxyServer::SetDefaultPool(std::string pool_name) {
    config_.default_pool = pool_name;
    router_.SetDefaultPool(std::move(pool_name));
}

std::shared_ptr<Backend> L4ProxyServer::SelectBackendFromPool(const std::string& pool_name) {
    auto it = named_pools_.find(pool_name);
    if (it != named_pools_.end() && it->second.lb) {
        return it->second.lb->SelectBackend();
    }
    if (lb_) {
        return lb_->SelectBackend();
    }
    return nullptr;
}

BackendConnectionPool* L4ProxyServer::GetPoolForName(const std::string& pool_name) noexcept {
    auto it = named_pools_.find(pool_name);
    if (it != named_pools_.end()) {
        return it->second.pool.get();
    }
    return backend_pool_.get();
}

void L4ProxyServer::EnableConnectionPooling(BackendConnectionPoolConfig config) {
    config_.enable_connection_pooling = true;
    config_.connection_pool_config = config;
    backend_pool_ = std::make_unique<BackendConnectionPool>(config);
}

void L4ProxyServer::DisableConnectionPooling() noexcept {
    config_.enable_connection_pooling = false;
    if (backend_pool_) {
        backend_pool_->Clear();
        backend_pool_.reset();
    }
}

bool L4ProxyServer::Start() {
    if (running_) return true;

    if (!listener_.BindAndListen(config_.listen_host.c_str(), config_.listen_port, 128, config_.reuse_port)) {
        return false;
    }

    running_ = true;
    ScheduleAccept();
    return true;
}

void L4ProxyServer::CloseListener() {
    listener_.Close();
    if (accept_in_flight_) {
        cancel_accept_ctx_.role = ProxyOpRole::Cancel;
        engine_.PrepCancel(&accept_ctx_, &cancel_accept_ctx_);
        engine_.Submit();
    }
}

void L4ProxyServer::Stop() {
    if (!running_) return;

    running_ = false;
    CloseListener();

    if (backend_pool_) {
        backend_pool_->Clear();
    }
    for (auto& [name, np] : named_pools_) {
        if (np.pool) {
            np.pool->Clear();
        }
    }

    // Initiate close on all active connections
    std::vector<uint64_t> conn_ids;
    conn_ids.reserve(connections_.size());
    for (const auto& [id, state_data] : connections_) {
        conn_ids.push_back(id);
    }

    for (uint64_t id : conn_ids) {
        auto it = connections_.find(id);
        if (it != connections_.end() && it->second->conn) {
            it->second->conn->InitiateClose("Proxy shutdown");
        }
    }

    // Drain remaining CQEs until all connections are cleaned up
    auto shutdown_start = std::chrono::steady_clock::now();
    while (!connections_.empty() && engine_.InFlightOps() > 0) {
        engine_.SubmitAndWait(1);
        std::array<CompletionEvent, 128> events{};
        uint32_t reaped = engine_.ReapCompletions(events);
        for (uint32_t i = 0; i < reaped; ++i) {
            if (!events[i].context) continue;
            auto* pctx = static_cast<ProxyOpContext*>(events[i].context);
            if (pctx->role == ProxyOpRole::Cancel) {
                if (pctx->conn) {
                    pctx->conn->DecrementPendingCqe();
                }
                continue;
            }
            if (pctx->role == ProxyOpRole::Accept) {
                accept_in_flight_ = false;
                if (events[i].result > 0) {
                    ::close(events[i].result);
                }
                continue;
            }

            auto conn_id = pctx->conn ? pctx->conn->GetId() : 0;
            auto it = connections_.find(conn_id);
            if (it != connections_.end()) {
                switch (pctx->role) {
                    case ProxyOpRole::Connect: HandleConnectCompletion(*it->second, events[i]); break;
                    case ProxyOpRole::ClientRead: HandleClientReadCompletion(*it->second, events[i]); break;
                    case ProxyOpRole::BackendWrite: HandleBackendWriteCompletion(*it->second, events[i]); break;
                    case ProxyOpRole::BackendRead: HandleBackendReadCompletion(*it->second, events[i]); break;
                    case ProxyOpRole::ClientWrite: HandleClientWriteCompletion(*it->second, events[i]); break;
                    default: break;
                }
            }
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - shutdown_start);
        if (elapsed > std::chrono::milliseconds(2000)) {
            break; // Force timeout break if sockets pending
        }
    }

    connections_.clear();
    paused_connections_.clear();
    active_connections_.store(0, std::memory_order_relaxed);
}

int L4ProxyServer::RunOnce(std::chrono::milliseconds timeout) {
    if (!running_ && connections_.empty()) return 0;

    (void)timeout;
    int submitted = engine_.Submit();
    uint32_t total_reaped = 0;
    while (true) {
        std::array<CompletionEvent, 256> events{};
        uint32_t reaped = engine_.ReapCompletions(events);
        if (reaped == 0) break;

        for (uint32_t i = 0; i < reaped; ++i) {
            if (!events[i].context) continue;
            
            if (events[i].context->on_complete) {
                events[i].context->on_complete(events[i]);
                continue;
            }

            auto* pctx = static_cast<ProxyOpContext*>(events[i].context);

            if (pctx->role == ProxyOpRole::Cancel) {
                if (pctx->conn) {
                    pctx->conn->DecrementPendingCqe();
                }
                continue;
            }

            if (pctx->role == ProxyOpRole::Accept) {
                HandleAcceptCompletion(events[i]);
                continue;
            }

            auto conn_id = pctx->conn ? pctx->conn->GetId() : 0;
            auto it = connections_.find(conn_id);
            if (it != connections_.end()) {
                switch (pctx->role) {
                    case ProxyOpRole::Connect: HandleConnectCompletion(*it->second, events[i]); break;
                    case ProxyOpRole::ClientRead: HandleClientReadCompletion(*it->second, events[i]); break;
                    case ProxyOpRole::BackendWrite: HandleBackendWriteCompletion(*it->second, events[i]); break;
                    case ProxyOpRole::BackendRead: HandleBackendReadCompletion(*it->second, events[i]); break;
                    case ProxyOpRole::ClientWrite: HandleClientWriteCompletion(*it->second, events[i]); break;
                    default: break;
                }
            }
        }
        total_reaped += reaped;
        
        // Batch submit any SQEs prepped during this reap cycle
        engine_.Submit();
    }

    auto now = std::chrono::steady_clock::now();
    if (now - last_timeout_check_ >= timeout_check_interval_) {
        last_timeout_check_ = now;
        CheckTimeouts(now);
    }
    ResumePausedReads();
    engine_.Submit();

    if (metrics_) {
        if (submitted > 0) {
            metrics_->iouring_sqe_submitted_total.fetch_add(static_cast<uint64_t>(submitted), std::memory_order_relaxed);
        }
        metrics_->iouring_cqe_reaped_total.fetch_add(total_reaped, std::memory_order_relaxed);
        metrics_->buffer_pool_utilization.store(buffer_pool_.UtilizationRatio(), std::memory_order_relaxed);
    }

    return static_cast<int>(total_reaped) + submitted;
}

void L4ProxyServer::ScheduleAccept() {
    if (accept_in_flight_ || !running_ || !listener_.IsListening()) return;

    // Pause accept if buffer pool high watermark reached or exhausted
    if (buffer_pool_.IsHighWatermarkReached() || buffer_pool_.IsExhausted()) {
        if (metrics_) {
            metrics_->backpressure_pause_events_total.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    accept_ctx_.role = ProxyOpRole::Accept;
    accept_client_addr_len_ = sizeof(sockaddr_in);

    if (listener_.AsyncAccept(&accept_client_addr_, &accept_client_addr_len_, &accept_ctx_)) {
        accept_in_flight_ = true;
        engine_.Submit();
    }
}

void L4ProxyServer::HandleAcceptCompletion(const CompletionEvent& event) {
    accept_in_flight_ = false;

    if (!running_ || event.result < 0) {
        if (running_) ScheduleAccept();
        return;
    }

    int client_fd = event.result;
    total_accepted_connections_.fetch_add(1, std::memory_order_relaxed);
    SocketUtils::SetTcpNoDelay(client_fd);

    if (config_.enable_l7_routing) {
        uint64_t conn_id = next_conn_id_++;
        auto conn = std::make_shared<ConnectionPair>(conn_id, engine_, client_fd);
        conn->SetBackendFd(-1);

        conn->SetCleanupCallback([this](uint64_t id) {
            connections_.erase(id);
            paused_connections_.erase(id);
            active_connections_.fetch_sub(1, std::memory_order_relaxed);
        });

        auto data = std::make_unique<ConnectionStateData>();
        data->conn = conn;
        data->l7_routed = false;

        char client_ip_buf[INET_ADDRSTRLEN] = "127.0.0.1";
        inet_ntop(AF_INET, &(accept_client_addr_.sin_addr), client_ip_buf, INET_ADDRSTRLEN);
        data->client_ip = client_ip_buf;
        data->client_port = ntohs(accept_client_addr_.sin_port);
        data->access_logger = access_logger_;
        data->metrics = metrics_;

        if (metrics_) {
            metrics_->requests_total.fetch_add(1, std::memory_order_relaxed);
            metrics_->active_connections.fetch_add(1, std::memory_order_relaxed);
        }

        data->client_read_ctx.role = ProxyOpRole::ClientRead;
        data->client_read_ctx.conn = conn;

        data->client_write_ctx.role = ProxyOpRole::ClientWrite;
        data->client_write_ctx.conn = conn;

        data->backend_read_ctx.role = ProxyOpRole::BackendRead;
        data->backend_read_ctx.conn = conn;

        data->backend_write_ctx.role = ProxyOpRole::BackendWrite;
        data->backend_write_ctx.conn = conn;

        data->cancel_backend_read_ctx.role = ProxyOpRole::Cancel;
        data->cancel_backend_read_ctx.conn = conn;

        TryStartClientRead(*data);
        engine_.Submit();

        connections_[conn_id] = std::move(data);
        active_connections_.fetch_add(1, std::memory_order_relaxed);

        ScheduleAccept();
        return;
    }

    auto backend = lb_->SelectBackend();
    if (!backend) {
        // No backend available
        ::close(client_fd);
        ScheduleAccept();
        return;
    }

    if (!backend->GetCircuitBreaker().AllowRequest()) {
        if (backend_pool_) {
            backend_pool_->Purge(backend->GetId());
        }
        ::close(client_fd);
        ScheduleAccept();
        return;
    }

    sockaddr_in backend_addr = backend->GetSockAddr();

    int backend_fd = -1;
    bool is_pooled = false;
    if (backend_pool_) {
        auto pooled_opt = backend_pool_->Acquire(backend->GetId());
        if (pooled_opt) {
            backend_fd = *pooled_opt;
            is_pooled = true;
            if (metrics_) {
                metrics_->backend_pool_hits_total.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    if (backend_fd < 0) {
        backend_fd = SocketUtils::CreateTcpSocket();
        if (backend_fd < 0) {
            ::close(client_fd);
            ScheduleAccept();
            return;
        }
        SocketUtils::SetTcpNoDelay(backend_fd);
        if (metrics_) {
            metrics_->backend_pool_misses_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

    uint64_t conn_id = next_conn_id_++;
    auto conn = std::make_shared<ConnectionPair>(conn_id, engine_, client_fd);
    conn->SetBackendFd(backend_fd);

    if (backend_pool_) {
        conn->SetBackendRecycleCallback([this, backend_id = backend->GetId()](int fd) -> bool {
            if (!backend_pool_) {
                return false;
            }
            return backend_pool_->Release(backend_id, fd);
        });
    }

    conn->SetCleanupCallback([this](uint64_t id) {
        connections_.erase(id);
        paused_connections_.erase(id);
        active_connections_.fetch_sub(1, std::memory_order_relaxed);
    });

    auto data = std::make_unique<ConnectionStateData>();
    data->conn = conn;
    data->backend_addr = backend_addr;
    data->backend = backend;
    backend->IncrementActiveConnections();

    // Populate access logging metadata
    char client_ip_buf[INET_ADDRSTRLEN] = "127.0.0.1";
    inet_ntop(AF_INET, &(accept_client_addr_.sin_addr), client_ip_buf, INET_ADDRSTRLEN);
    data->client_ip = client_ip_buf;
    data->client_port = ntohs(accept_client_addr_.sin_port);
    data->upstream_endpoint = backend->GetHost() + ":" + std::to_string(backend->GetPort());
    data->access_logger = access_logger_;
    data->metrics = metrics_;

    if (metrics_) {
        metrics_->requests_total.fetch_add(1, std::memory_order_relaxed);
        metrics_->active_connections.fetch_add(1, std::memory_order_relaxed);
    }

    data->client_read_ctx.role = ProxyOpRole::ClientRead;
    data->client_read_ctx.conn = conn;

    data->client_write_ctx.role = ProxyOpRole::ClientWrite;
    data->client_write_ctx.conn = conn;

    data->backend_read_ctx.role = ProxyOpRole::BackendRead;
    data->backend_read_ctx.conn = conn;

    data->backend_write_ctx.role = ProxyOpRole::BackendWrite;
    data->backend_write_ctx.conn = conn;

    data->cancel_backend_read_ctx.role = ProxyOpRole::Cancel;
    data->cancel_backend_read_ctx.conn = conn;

    if (is_pooled) {
        conn->TransitionTo(ConnectionState::Established);
        TryStartClientRead(*data);
        TryStartBackendRead(*data);
        engine_.Submit();
    } else {
        data->connect_ctx.role = ProxyOpRole::Connect;
        data->connect_ctx.conn = conn;

        conn->TransitionTo(ConnectionState::ConnectingBackend);
        engine_.PrepConnect(backend_fd, reinterpret_cast<const sockaddr*>(&backend_addr), sizeof(backend_addr), &data->connect_ctx);
        conn->IncrementPendingCqe();
        engine_.Submit();
    }

    connections_[conn_id] = std::move(data);
    active_connections_.fetch_add(1, std::memory_order_relaxed);

    ScheduleAccept();
}

void L4ProxyServer::HandleConnectCompletion(ConnectionStateData& data, const CompletionEvent& event) {
    auto conn = data.conn;
    auto backend = data.backend;

    if (event.result < 0) {
        if (backend) {
            backend->ReportConnectionFailure();
            auto* pool_ptr = GetPoolForName(data.matched_pool);
            if (pool_ptr) {
                pool_ptr->Purge(backend->GetId());
            }
        }
        data.status_code = 502;
        conn->InitiateClose("Backend connect failed");
        conn->DecrementPendingCqe();
        return;
    }

    if (conn->IsDraining()) {
        conn->InitiateClose("Proxy draining");
        conn->DecrementPendingCqe();
        return;
    }

    if (backend) {
        backend->ReportConnectionSuccess();
    }

    conn->TransitionTo(ConnectionState::Established);

    if (data.backend_write_ctx.buf) {
        Buffer* bbuf = data.backend_write_ctx.buf;
        conn->SetBackendReusable(false);
        engine_.PrepWrite(conn->GetBackendFd(), bbuf->ReadPtr(), bbuf->ReadableBytes(), 0, &data.backend_write_ctx);
        conn->IncrementPendingCqe();
    } else {
        TryStartClientRead(data);
    }
    TryStartBackendRead(data);
    engine_.Submit();

    conn->DecrementPendingCqe();
}

void L4ProxyServer::SetClientReadPaused(ConnectionStateData& data, bool paused) {
    data.client_read_paused = paused;
    if (paused) {
        if (data.conn) {
            paused_connections_.insert(data.conn->GetId());
        }
    } else if (!data.backend_read_paused && data.conn) {
        paused_connections_.erase(data.conn->GetId());
    }
}

void L4ProxyServer::SetBackendReadPaused(ConnectionStateData& data, bool paused) {
    data.backend_read_paused = paused;
    if (paused) {
        if (data.conn) {
            paused_connections_.insert(data.conn->GetId());
        }
    } else if (!data.client_read_paused && data.conn) {
        paused_connections_.erase(data.conn->GetId());
    }
}

void L4ProxyServer::TryStartClientRead(ConnectionStateData& data) {
    if (data.client_read_active || data.conn->IsClientReadStopped() || data.conn->IsDraining()) {
        SetClientReadPaused(data, false);
        return;
    }

    if (data.backend_read_paused) {
        SetClientReadPaused(data, true);
        return;
    }

    Buffer* buf = buffer_pool_.Acquire();
    if (!buf) {
        SetClientReadPaused(data, true);
        return;
    }

    SetClientReadPaused(data, false);
    data.client_read_ctx.buf = buf;
    data.client_read_active = true;

    engine_.PrepRead(data.conn->GetClientFd(), buf->data, buf->capacity, 0, &data.client_read_ctx);
    data.conn->IncrementPendingCqe();
}

void L4ProxyServer::HandleClientReadCompletion(ConnectionStateData& data, const CompletionEvent& event) {
    data.client_read_active = false;
    auto conn = data.conn;

    Buffer* buf = data.client_read_ctx.buf;
    data.client_read_ctx.buf = nullptr;

    if (buf) {
        if (config_.enable_l7_routing && !data.l7_routed) {
            if (event.result > 0) {
                data.bytes_received += static_cast<uint64_t>(event.result);
                if (metrics_) {
                    metrics_->bytes_read_total.fetch_add(static_cast<uint64_t>(event.result), std::memory_order_relaxed);
                }
                data.raw_request_accumulated.append(reinterpret_cast<const char*>(buf->data), static_cast<size_t>(event.result));
                buffer_pool_.Release(buf);
                buf = nullptr;

                data.parser.Reset();
                ParseResult parse_res = data.parser.Parse(data.raw_request_accumulated);

                if (parse_res.IsError() || (parse_res.IsIncomplete() && data.raw_request_accumulated.size() > kDefaultMaxHeaderSize)) {
                    static constexpr std::string_view k400Response =
                        "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 15\r\n"
                        "Connection: close\r\n\r\n"
                        "400 Bad Request";
                    ::send(conn->GetClientFd(), k400Response.data(), k400Response.size(), MSG_NOSIGNAL);
                    data.status_code = 400;
                    conn->InitiateClose("HTTP Bad Request");
                    conn->DecrementPendingCqe();
                    return;
                }

                if (parse_res.IsIncomplete()) {
                    TryStartClientRead(data);
                    engine_.Submit();
                    conn->DecrementPendingCqe();
                    return;
                }

                // Headers complete! Perform L7 routing
                const auto& req = data.parser.GetRequest();
                auto host_hdr = req.GetHeader("Host");
                std::string_view host = host_hdr.value_or("");
                auto colon_pos = host.find(':');
                if (colon_pos != std::string_view::npos) {
                    host = host.substr(0, colon_pos);
                }
                std::string_view path = req.uri;

                auto match_opt = router_.Match(host, path);
                if (!match_opt) {
                    static constexpr std::string_view k404Response =
                        "HTTP/1.1 404 Not Found\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 13\r\n"
                        "Connection: close\r\n\r\n"
                        "404 Not Found";
                    ::send(conn->GetClientFd(), k404Response.data(), k404Response.size(), MSG_NOSIGNAL);
                    data.status_code = 404;
                    conn->InitiateClose("HTTP Route Not Found");
                    conn->DecrementPendingCqe();
                    return;
                }

                data.l7_routed = true;
                data.matched_pool = match_opt->target_pool;
                data.method = std::string(req.method_str);
                data.uri = std::string(req.uri);
                data.http_version = "HTTP/" + std::to_string(req.http_major) + "." + std::to_string(req.http_minor);
                data.parsed_request = true;

                HeaderInjectionOptions inj_opts;
                inj_opts.client_ip = data.client_ip;
                inj_opts.client_proto = "http";
                inj_opts.host = std::string(host);
                if (match_opt->route && match_opt->route->strip_prefix) {
                    inj_opts.rewritten_path = match_opt->rewritten_path;
                }
                if (match_opt->route && !match_opt->route->inject_headers) {
                    inj_opts.inject_forwarded_for = false;
                    inj_opts.inject_forwarded_proto = false;
                    inj_opts.inject_forwarded_host = false;
                    inj_opts.inject_request_id = false;
                }
                std::string rewritten_request = HttpRewriter::RewriteRequest(data.raw_request_accumulated, inj_opts);

                auto backend = SelectBackendFromPool(data.matched_pool);
                if (!backend) {
                    static constexpr std::string_view k503Response =
                        "HTTP/1.1 503 Service Unavailable\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 23\r\n"
                        "Connection: close\r\n\r\n"
                        "503 Service Unavailable";
                    ::send(conn->GetClientFd(), k503Response.data(), k503Response.size(), MSG_NOSIGNAL);
                    data.status_code = 503;
                    conn->InitiateClose("No backend available in pool");
                    conn->DecrementPendingCqe();
                    return;
                }

                if (!backend->GetCircuitBreaker().AllowRequest()) {
                    auto* pool_ptr = GetPoolForName(data.matched_pool);
                    if (pool_ptr) {
                        pool_ptr->Purge(backend->GetId());
                    }
                    static constexpr std::string_view k503Response =
                        "HTTP/1.1 503 Service Unavailable\r\n"
                        "Content-Type: text/plain\r\n"
                        "Content-Length: 23\r\n"
                        "Connection: close\r\n\r\n"
                        "503 Service Unavailable";
                    ::send(conn->GetClientFd(), k503Response.data(), k503Response.size(), MSG_NOSIGNAL);
                    data.status_code = 503;
                    conn->InitiateClose("Circuit breaker open");
                    conn->DecrementPendingCqe();
                    return;
                }

                data.backend = backend;
                data.backend_addr = backend->GetSockAddr();
                data.upstream_endpoint = backend->GetHost() + ":" + std::to_string(backend->GetPort());
                backend->IncrementActiveConnections();

                int backend_fd = -1;
                bool is_pooled = false;
                auto* pool_ptr = GetPoolForName(data.matched_pool);
                if (pool_ptr) {
                    auto pooled_opt = pool_ptr->Acquire(backend->GetId());
                    if (pooled_opt) {
                        backend_fd = *pooled_opt;
                        is_pooled = true;
                        if (metrics_) {
                            metrics_->backend_pool_hits_total.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }

                if (backend_fd < 0) {
                    backend_fd = SocketUtils::CreateTcpSocket();
                    if (backend_fd < 0) {
                        static constexpr std::string_view k500Response =
                            "HTTP/1.1 500 Internal Server Error\r\n"
                            "Content-Length: 0\r\n"
                            "Connection: close\r\n\r\n";
                        ::send(conn->GetClientFd(), k500Response.data(), k500Response.size(), MSG_NOSIGNAL);
                        data.status_code = 500;
                        conn->InitiateClose("Failed to create backend socket");
                        conn->DecrementPendingCqe();
                        return;
                    }
                    SocketUtils::SetTcpNoDelay(backend_fd);
                    if (metrics_) {
                        metrics_->backend_pool_misses_total.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                conn->SetBackendFd(backend_fd);

                if (pool_ptr) {
                    conn->SetBackendRecycleCallback([this, pool_name = data.matched_pool, backend_id = backend->GetId()](int fd) -> bool {
                        auto* p = GetPoolForName(pool_name);
                        if (!p) return false;
                        return p->Release(backend_id, fd);
                    });
                }

                Buffer* out_buf = buffer_pool_.Acquire();
                if (!out_buf) {
                    conn->InitiateClose("Buffer pool exhausted");
                    conn->DecrementPendingCqe();
                    return;
                }

                size_t to_write = std::min(out_buf->capacity, rewritten_request.size());
                std::memcpy(out_buf->data, rewritten_request.data(), to_write);
                out_buf->AdvanceWrite(to_write);
                data.backend_write_ctx.buf = out_buf;

                if (is_pooled) {
                    conn->TransitionTo(ConnectionState::Established);
                    conn->SetBackendReusable(false);
                    engine_.PrepWrite(conn->GetBackendFd(), out_buf->ReadPtr(), out_buf->ReadableBytes(), 0, &data.backend_write_ctx);
                    conn->IncrementPendingCqe();
                    TryStartBackendRead(data);
                    engine_.Submit();
                } else {
                    data.connect_ctx.role = ProxyOpRole::Connect;
                    data.connect_ctx.conn = conn;

                    conn->TransitionTo(ConnectionState::ConnectingBackend);
                    engine_.PrepConnect(backend_fd, reinterpret_cast<const sockaddr*>(&data.backend_addr), sizeof(data.backend_addr), &data.connect_ctx);
                    conn->IncrementPendingCqe();
                    engine_.Submit();
                }

                conn->DecrementPendingCqe();
                return;
            } else if (event.result == 0) {
                buffer_pool_.Release(buf);
                conn->InitiateClose("Client closed before sending request");
                conn->DecrementPendingCqe();
                return;
            } else if (event.result == -EAGAIN || event.result == -EWOULDBLOCK) {
                data.client_read_ctx.buf = buf;
                engine_.PrepRead(conn->GetClientFd(), buf->data, buf->capacity, 0, &data.client_read_ctx);
                conn->IncrementPendingCqe();
                data.client_read_active = true;
                conn->DecrementPendingCqe();
                return;
            } else {
                buffer_pool_.Release(buf);
                conn->InitiateClose("Client read error");
                conn->DecrementPendingCqe();
                return;
            }
        }

        if (event.result > 0) {
            data.bytes_received += static_cast<uint64_t>(event.result);
            if (metrics_) {
                metrics_->bytes_read_total.fetch_add(static_cast<uint64_t>(event.result), std::memory_order_relaxed);
            }
            if (!data.parsed_request) {
                data.parsed_request = true;
                std::string_view req(reinterpret_cast<const char*>(buf->data), static_cast<size_t>(event.result));
                auto line_end = req.find("\r\n");
                if (line_end != std::string_view::npos) {
                    std::string_view line = req.substr(0, line_end);
                    auto sp1 = line.find(' ');
                    if (sp1 != std::string_view::npos) {
                        data.method = std::string(line.substr(0, sp1));
                        auto sp2 = line.find(' ', sp1 + 1);
                        if (sp2 != std::string_view::npos) {
                            data.uri = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));
                            data.http_version = std::string(line.substr(sp2 + 1));
                        } else {
                            data.uri = std::string(line.substr(sp1 + 1));
                        }
                    }
                }
            }

            buf->AdvanceWrite(static_cast<size_t>(event.result));
            data.backend_write_ctx.buf = buf;
            conn->SetBackendReusable(false);
            engine_.PrepWrite(conn->GetBackendFd(), buf->ReadPtr(), buf->ReadableBytes(), 0, &data.backend_write_ctx);
            conn->IncrementPendingCqe();
            engine_.Submit();
        } else if (event.result == 0) {
            // EOF from client
            buffer_pool_.Release(buf);
            if (backend_pool_ && conn->IsBackendReusable()) {
                if (data.backend_read_active) {
                    if (engine_.PrepCancel(&data.backend_read_ctx, &data.cancel_backend_read_ctx)) {
                        conn->IncrementPendingCqe();
                        engine_.Submit();
                    }
                }
                conn->InitiateClose("Client closed reusable connection");
            } else {
                conn->HandleClientFin();
            }
        } else if (event.result == -EAGAIN || event.result == -EWOULDBLOCK) {
            data.client_read_ctx.buf = buf;
            engine_.PrepRead(conn->GetClientFd(), buf->data, buf->capacity, 0, &data.client_read_ctx);
            conn->IncrementPendingCqe();
            data.client_read_active = true;
            conn->DecrementPendingCqe();
            return;
        } else {
            // Error on client read
            buffer_pool_.Release(buf);
            if (event.result == -ECONNRESET && backend_pool_ && conn->IsBackendReusable()) {
                if (data.backend_read_active) {
                    if (engine_.PrepCancel(&data.backend_read_ctx, &data.cancel_backend_read_ctx)) {
                        conn->IncrementPendingCqe();
                        engine_.Submit();
                    }
                }
                conn->InitiateClose("Client reset reusable connection");
            } else if (event.result != -ECONNRESET) {
                std::cerr << "Client read error: " << event.result << "\n";
                conn->InitiateClose("Client read error");
            } else {
                conn->HandleClientFin();
            }
        }
    }

    conn->DecrementPendingCqe();
}

void L4ProxyServer::HandleBackendWriteCompletion(ConnectionStateData& data, const CompletionEvent& event) {
    auto conn = data.conn;
    Buffer* buf = data.backend_write_ctx.buf;

    if (buf) {
        if (event.result > 0) {
            buf->AdvanceRead(static_cast<size_t>(event.result));
            if (buf->ReadableBytes() > 0) {
                // Partial write: re-issue write for remaining bytes
                engine_.PrepWrite(conn->GetBackendFd(), buf->ReadPtr(), buf->ReadableBytes(), 0, &data.backend_write_ctx);
                conn->IncrementPendingCqe();
                engine_.Submit();
                conn->DecrementPendingCqe();
                return;
            }

            // Complete payload write succeeded
            buffer_pool_.Release(buf);
            data.backend_write_ctx.buf = nullptr;
            TryStartClientRead(data);
        } else if (event.result == -EAGAIN || event.result == -EWOULDBLOCK) {
            data.backend_write_ctx.buf = buf;
            engine_.PrepWrite(conn->GetBackendFd(), buf->ReadPtr(), buf->ReadableBytes(), 0, &data.backend_write_ctx);
            conn->IncrementPendingCqe();
            conn->DecrementPendingCqe();
            return;
        } else {
            // Backend write error
            buffer_pool_.Release(buf);
            data.backend_write_ctx.buf = nullptr;
            conn->SetBackendReusable(false);
            conn->InitiateClose("Backend write error");
        }
    }

    conn->DecrementPendingCqe();
}

void L4ProxyServer::TryStartBackendRead(ConnectionStateData& data) {
    if (data.backend_read_active || data.conn->IsBackendReadStopped() || data.conn->IsDraining()) {
        SetBackendReadPaused(data, false);
        return;
    }

    Buffer* buf = buffer_pool_.Acquire();
    if (!buf) {
        SetBackendReadPaused(data, true);
        return;
    }

    SetBackendReadPaused(data, false);
    data.backend_read_ctx.buf = buf;
    data.backend_read_active = true;

    engine_.PrepRead(data.conn->GetBackendFd(), buf->data, buf->capacity, 0, &data.backend_read_ctx);
    data.conn->IncrementPendingCqe();
}

void L4ProxyServer::HandleBackendReadCompletion(ConnectionStateData& data, const CompletionEvent& event) {
    data.backend_read_active = false;
    auto conn = data.conn;

    Buffer* buf = data.backend_read_ctx.buf;
    data.backend_read_ctx.buf = nullptr;

    if (buf) {
        if (event.result > 0) {
            data.bytes_sent += static_cast<uint64_t>(event.result);
            if (metrics_) {
                metrics_->bytes_written_total.fetch_add(static_cast<uint64_t>(event.result), std::memory_order_relaxed);
            }
            if (!data.parsed_response) {
                data.parsed_response = true;
                std::string_view resp(reinterpret_cast<const char*>(buf->data), static_cast<size_t>(event.result));
                if (resp.rfind("HTTP/", 0) == 0) {
                    auto sp1 = resp.find(' ');
                    if (sp1 != std::string_view::npos && sp1 + 4 <= resp.size()) {
                        int code = 0;
                        for (size_t k = sp1 + 1; k < sp1 + 4 && k < resp.size(); ++k) {
                            if (resp[k] >= '0' && resp[k] <= '9') {
                                code = code * 10 + (resp[k] - '0');
                            }
                        }
                        if (code >= 100 && code <= 599) {
                            data.status_code = code;
                        }
                    }
                }
            }

            buf->AdvanceWrite(static_cast<size_t>(event.result));
            data.client_write_ctx.buf = buf;
            engine_.PrepWrite(conn->GetClientFd(), buf->ReadPtr(), buf->ReadableBytes(), 0, &data.client_write_ctx);
            conn->IncrementPendingCqe();
            engine_.Submit();
        } else if (event.result == 0) {
            // EOF from backend
            buffer_pool_.Release(buf);
            conn->SetBackendReusable(false);
            conn->HandleBackendFin();
        } else if (event.result == -EAGAIN || event.result == -EWOULDBLOCK) {
            data.backend_read_ctx.buf = buf;
            engine_.PrepRead(conn->GetBackendFd(), buf->data, buf->capacity, 0, &data.backend_read_ctx);
            conn->IncrementPendingCqe();
            data.backend_read_active = true;
            conn->DecrementPendingCqe();
            return;
        } else if (event.result == -ECANCELED) {
            // Backend read cancelled (recycled connection)
            buffer_pool_.Release(buf);
        } else {
            // Error on backend read
            buffer_pool_.Release(buf);
            conn->SetBackendReusable(false);
            if (event.result != -ECONNRESET) {
                std::cerr << "Backend read error: " << event.result << "\n";
                conn->InitiateClose("Backend read error");
            } else {
                conn->HandleBackendFin();
            }
        }
    }

    conn->DecrementPendingCqe();
}

void L4ProxyServer::HandleClientWriteCompletion(ConnectionStateData& data, const CompletionEvent& event) {
    auto conn = data.conn;
    Buffer* buf = data.client_write_ctx.buf;

    if (buf) {
        if (event.result > 0) {
            buf->AdvanceRead(static_cast<size_t>(event.result));
            if (buf->ReadableBytes() > 0) {
                // Partial write: re-issue write for remaining bytes
                engine_.PrepWrite(conn->GetClientFd(), buf->ReadPtr(), buf->ReadableBytes(), 0, &data.client_write_ctx);
                conn->IncrementPendingCqe();
                engine_.Submit();
                conn->DecrementPendingCqe();
                return;
            }

            // Complete payload write succeeded
            buffer_pool_.Release(buf);
            data.client_write_ctx.buf = nullptr;
            if (backend_pool_ && !conn->IsBackendReadStopped()) {
                conn->SetBackendReusable(true);
            }
            TryStartBackendRead(data);
        } else if (event.result == -EAGAIN || event.result == -EWOULDBLOCK) {
            data.client_write_ctx.buf = buf;
            engine_.PrepWrite(conn->GetClientFd(), buf->ReadPtr(), buf->ReadableBytes(), 0, &data.client_write_ctx);
            conn->IncrementPendingCqe();
            conn->DecrementPendingCqe();
            return;
        } else {
            // Client write error
            buffer_pool_.Release(buf);
            data.client_write_ctx.buf = nullptr;
            conn->SetBackendReusable(false);
            conn->InitiateClose("Client write error");
        }
    }

    conn->DecrementPendingCqe();
}

void L4ProxyServer::ResumePausedReads() {
    if (paused_connections_.empty() || buffer_pool_.FreeCount() == 0) {
        ScheduleAccept();
        return;
    }

    std::vector<uint64_t> to_resume(paused_connections_.begin(), paused_connections_.end());
    for (uint64_t id : to_resume) {
        if (buffer_pool_.FreeCount() == 0) {
            break;
        }
        auto it = connections_.find(id);
        if (it == connections_.end()) {
            paused_connections_.erase(id);
            continue;
        }
        auto& data = *it->second;
        if (data.backend_read_paused) {
            TryStartBackendRead(data);
        }
        if (buffer_pool_.FreeCount() == 0) {
            break;
        }
        if (data.client_read_paused) {
            TryStartClientRead(data);
        }
    }
    ScheduleAccept();
}

void L4ProxyServer::CheckTimeouts(std::chrono::steady_clock::time_point now) {
    if (backend_pool_) {
        backend_pool_->ReapExpired(now);
    }
    for (auto& [name, np] : named_pools_) {
        if (np.pool) {
            np.pool->ReapExpired(now);
        }
    }

    if (connections_.empty()) return;

    for (auto& [id, data] : connections_) {
        if (!data || !data->conn) continue;
        data->conn->CheckHalfCloseTimeout(now, config_.half_close_timeout);
        data->conn->CheckIdleTimeout(now, config_.idle_timeout);
    }
}

void L4ProxyServer::CheckTimeouts() {
    CheckTimeouts(std::chrono::steady_clock::now());
}

} // namespace helios
