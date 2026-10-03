#include "l4_proxy_server.hpp"
#include "socket_utils.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <array>
#include <vector>
#include <algorithm>
#include <iostream>

namespace helios {

L4ProxyServer::L4ProxyServer()
    : L4ProxyServer(Config{}) {}

L4ProxyServer::L4ProxyServer(Config config)
    : config_(std::move(config)),
      engine_(static_cast<uint32_t>(config_.ring_entries)),
      buffer_pool_(config_.total_buffers, config_.buffer_size),
      backpressure_ctrl_(buffer_pool_),
      listener_(engine_),
      lb_(std::make_unique<RoundRobinLoadBalancer>()) {
    accept_ctx_.role = ProxyOpRole::Accept;
}

L4ProxyServer::~L4ProxyServer() {
    Stop();
}

void L4ProxyServer::AddBackend(std::string host, uint16_t port) {
    lb_->AddBackend(std::make_shared<Backend>(next_backend_id_++, std::move(host), port));
}

bool L4ProxyServer::Start() {
    if (running_) return true;

    if (!listener_.BindAndListen(config_.listen_host.c_str(), config_.listen_port)) {
        return false;
    }

    running_ = true;
    ScheduleAccept();
    return true;
}

void L4ProxyServer::Stop() {
    if (!running_) return;

    running_ = false;
    listener_.Close();

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
            auto* pctx = static_cast<ProxyOpContext*>(events[i].context);

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

    CheckTimeouts();
    ResumePausedReads();
    engine_.Submit();

    return static_cast<int>(total_reaped) + submitted;
}

void L4ProxyServer::ScheduleAccept() {
    if (accept_in_flight_ || !running_) return;

    // Pause accept if buffer pool high watermark reached or exhausted
    if (buffer_pool_.IsHighWatermarkReached() || buffer_pool_.IsExhausted()) {
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
    auto backend = lb_->SelectBackend();
    if (!backend) {
        // No backend available
        ::close(client_fd);
        ScheduleAccept();
        return;
    }

    sockaddr_in backend_addr = backend->GetSockAddr();

    int backend_fd = SocketUtils::CreateTcpSocket();
    if (backend_fd < 0) {
        ::close(client_fd);
        ScheduleAccept();
        return;
    }

    uint64_t conn_id = next_conn_id_++;
    auto conn = std::make_shared<ConnectionPair>(conn_id, engine_, client_fd);
    conn->SetBackendFd(backend_fd);

    conn->SetCleanupCallback([this](uint64_t id) {
        connections_.erase(id);
    });

    auto data = std::make_unique<ConnectionStateData>();
    data->conn = conn;
    data->backend_addr = backend_addr;

    data->client_read_ctx.role = ProxyOpRole::ClientRead;
    data->client_read_ctx.conn = conn;

    data->client_write_ctx.role = ProxyOpRole::ClientWrite;
    data->client_write_ctx.conn = conn;

    data->backend_read_ctx.role = ProxyOpRole::BackendRead;
    data->backend_read_ctx.conn = conn;

    data->backend_write_ctx.role = ProxyOpRole::BackendWrite;
    data->backend_write_ctx.conn = conn;

    data->connect_ctx.role = ProxyOpRole::Connect;
    data->connect_ctx.conn = conn;

    conn->TransitionTo(ConnectionState::ConnectingBackend);

    engine_.PrepConnect(backend_fd, reinterpret_cast<const sockaddr*>(&backend_addr), sizeof(backend_addr), &data->connect_ctx);
    conn->IncrementPendingCqe();
    engine_.Submit();

    connections_[conn_id] = std::move(data);

    ScheduleAccept();
}

void L4ProxyServer::HandleConnectCompletion(ConnectionStateData& data, const CompletionEvent& event) {
    auto conn = data.conn;

    if (event.result < 0 || conn->IsDraining()) {
        conn->InitiateClose("Backend connect failed");
        conn->DecrementPendingCqe();
        return;
    }

    conn->TransitionTo(ConnectionState::Established);

    TryStartClientRead(data);
    TryStartBackendRead(data);
    engine_.Submit();

    conn->DecrementPendingCqe();
}

void L4ProxyServer::TryStartClientRead(ConnectionStateData& data) {
    if (data.client_read_active || data.conn->IsClientReadStopped() || data.conn->IsDraining()) {
        data.client_read_paused = false;
        return;
    }

    if (data.backend_read_paused) {
        data.client_read_paused = true;
        return;
    }

    Buffer* buf = buffer_pool_.Acquire();
    if (!buf) {
        data.client_read_paused = true;
        return;
    }

    data.client_read_paused = false;
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
        if (event.result > 0) {
            buf->AdvanceWrite(static_cast<size_t>(event.result));
            data.backend_write_ctx.buf = buf;
            engine_.PrepWrite(conn->GetBackendFd(), buf->ReadPtr(), buf->ReadableBytes(), 0, &data.backend_write_ctx);
            conn->IncrementPendingCqe();
            engine_.Submit();
        } else if (event.result == 0) {
            // EOF from client
            buffer_pool_.Release(buf);
            conn->HandleClientFin();
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
            if (event.result != -ECONNRESET) {
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
            data.client_read_paused = true;
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
            conn->InitiateClose("Backend write error");
        }
    }

    conn->DecrementPendingCqe();
}

void L4ProxyServer::TryStartBackendRead(ConnectionStateData& data) {
    if (data.backend_read_active || data.conn->IsBackendReadStopped() || data.conn->IsDraining()) {
        data.backend_read_paused = false;
        return;
    }

    Buffer* buf = buffer_pool_.Acquire();
    if (!buf) {
        data.backend_read_paused = true;
        return;
    }

    data.backend_read_paused = false;
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
            buf->AdvanceWrite(static_cast<size_t>(event.result));
            data.client_write_ctx.buf = buf;
            engine_.PrepWrite(conn->GetClientFd(), buf->ReadPtr(), buf->ReadableBytes(), 0, &data.client_write_ctx);
            conn->IncrementPendingCqe();
            engine_.Submit();
        } else if (event.result == 0) {
            // EOF from backend
            buffer_pool_.Release(buf);
            conn->HandleBackendFin();
        } else if (event.result == -EAGAIN || event.result == -EWOULDBLOCK) {
            data.backend_read_ctx.buf = buf;
            engine_.PrepRead(conn->GetBackendFd(), buf->data, buf->capacity, 0, &data.backend_read_ctx);
            conn->IncrementPendingCqe();
            data.backend_read_active = true;
            conn->DecrementPendingCqe();
            return;
        } else {
            // Error on backend read
            buffer_pool_.Release(buf);
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
            data.backend_read_paused = true;
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
            conn->InitiateClose("Client write error");
        }
    }

    conn->DecrementPendingCqe();
}

void L4ProxyServer::ResumePausedReads() {
    for (auto& [id, data] : connections_) {
        if (!data) continue;
        if (data->backend_read_paused) {
            TryStartBackendRead(*data);
        }
    }
    for (auto& [id, data] : connections_) {
        if (!data) continue;
        if (data->client_read_paused) {
            TryStartClientRead(*data);
        }
    }
    ScheduleAccept();
}

void L4ProxyServer::CheckTimeouts() {
    auto now = std::chrono::steady_clock::now();

    if (buffer_pool_.FreeCount() == 0) {
        size_t held = 0;
        for (auto& [id, data] : connections_) {
            if (!data) continue;
            if (data->client_read_ctx.buf) held++;
            if (data->client_write_ctx.buf) held++;
            if (data->backend_read_ctx.buf) held++;
            if (data->backend_write_ctx.buf) held++;
        }
        std::cerr << "POOL EXHAUSTED! Connections hold " << held << " buffers.\n";
    }
    for (auto& [id, data] : connections_) {
        if (!data || !data->conn) continue;
        data->conn->CheckHalfCloseTimeout(now, config_.half_close_timeout);
        data->conn->CheckIdleTimeout(now, config_.idle_timeout);
    }
}

} // namespace helios
