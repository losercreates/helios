#include "ring_engine.hpp"
#include <stdexcept>
#include <cstring>
#include <utility>

namespace helios {

RingEngine::RingEngine(uint32_t entries, uint32_t flags)
    : capacity_(entries) {
    std::memset(&ring_, 0, sizeof(ring_));
    int ret = io_uring_queue_init(entries, &ring_, flags);
    if (ret < 0) {
        throw std::runtime_error("Failed to initialize io_uring queue: errno " + std::to_string(-ret));
    }
    initialized_ = true;
}

RingEngine::~RingEngine() {
    Shutdown();
}

RingEngine::RingEngine(RingEngine&& other) noexcept
    : ring_(other.ring_),
      capacity_(other.capacity_),
      pending_sqes_(other.pending_sqes_),
      in_flight_ops_(other.in_flight_ops_),
      initialized_(other.initialized_) {
    std::memset(&other.ring_, 0, sizeof(other.ring_));
    other.capacity_ = 0;
    other.pending_sqes_ = 0;
    other.in_flight_ops_ = 0;
    other.initialized_ = false;
}

RingEngine& RingEngine::operator=(RingEngine&& other) noexcept {
    if (this != &other) {
        Shutdown();
        ring_ = other.ring_;
        capacity_ = other.capacity_;
        pending_sqes_ = other.pending_sqes_;
        in_flight_ops_ = other.in_flight_ops_;
        initialized_ = other.initialized_;

        std::memset(&other.ring_, 0, sizeof(other.ring_));
        other.capacity_ = 0;
        other.pending_sqes_ = 0;
        other.in_flight_ops_ = 0;
        other.initialized_ = false;
    }
    return *this;
}

struct io_uring_sqe* RingEngine::GetSqeOrNull() noexcept {
    if (!initialized_) {
        return nullptr;
    }
    return io_uring_get_sqe(&ring_);
}

void RingEngine::TagSqe(struct io_uring_sqe* sqe, OpContext* ctx) noexcept {
    if (!sqe || !ctx) return;
    io_uring_sqe_set_data(sqe, ctx);
    ctx->MarkInFlight();
    pending_sqes_++;
    in_flight_ops_++;
}

bool RingEngine::PrepNop(OpContext* ctx) {
    struct io_uring_sqe* sqe = GetSqeOrNull();
    if (!sqe) return false;
    io_uring_prep_nop(sqe);
    ctx->type = OpType::Nop;
    TagSqe(sqe, ctx);
    return true;
}

bool RingEngine::PrepRead(int fd, void* buf, size_t nbytes, uint64_t offset, OpContext* ctx) {
    struct io_uring_sqe* sqe = GetSqeOrNull();
    if (!sqe) return false;
    io_uring_prep_read(sqe, fd, buf, static_cast<unsigned int>(nbytes), offset);
    ctx->type = OpType::Read;
    ctx->fd = fd;
    ctx->buffer = buf;
    ctx->bytes_requested = nbytes;
    TagSqe(sqe, ctx);
    return true;
}

bool RingEngine::PrepWrite(int fd, const void* buf, size_t nbytes, uint64_t offset, OpContext* ctx) {
    struct io_uring_sqe* sqe = GetSqeOrNull();
    if (!sqe) return false;
    io_uring_prep_write(sqe, fd, buf, static_cast<unsigned int>(nbytes), offset);
    ctx->type = OpType::Write;
    ctx->fd = fd;
    ctx->buffer = const_cast<void*>(buf);
    ctx->bytes_requested = nbytes;
    TagSqe(sqe, ctx);
    return true;
}

bool RingEngine::PrepAccept(int listener_fd, sockaddr* addr, socklen_t* addrlen, int flags, OpContext* ctx) {
    struct io_uring_sqe* sqe = GetSqeOrNull();
    if (!sqe) return false;
    io_uring_prep_accept(sqe, listener_fd, addr, addrlen, flags);
    ctx->type = OpType::Accept;
    ctx->fd = listener_fd;
    TagSqe(sqe, ctx);
    return true;
}

bool RingEngine::PrepConnect(int fd, const sockaddr* addr, socklen_t addrlen, OpContext* ctx) {
    struct io_uring_sqe* sqe = GetSqeOrNull();
    if (!sqe) return false;
    io_uring_prep_connect(sqe, fd, addr, addrlen);
    ctx->type = OpType::Connect;
    ctx->fd = fd;
    TagSqe(sqe, ctx);
    return true;
}

bool RingEngine::PrepCancel(OpContext* target_ctx, OpContext* cancel_ctx) {
    struct io_uring_sqe* sqe = GetSqeOrNull();
    if (!sqe) return false;
    io_uring_prep_cancel(sqe, target_ctx, 0);
    cancel_ctx->type = OpType::Cancel;
    TagSqe(sqe, cancel_ctx);
    return true;
}

int RingEngine::Submit() {
    if (!initialized_) return -1;
    int ret = io_uring_submit(&ring_);
    if (ret > 0) {
        uint32_t submitted = static_cast<uint32_t>(ret);
        pending_sqes_ = (pending_sqes_ >= submitted) ? (pending_sqes_ - submitted) : 0;
    }
    return ret;
}

int RingEngine::SubmitAndWait(uint32_t wait_nr) {
    if (!initialized_) return -1;
    int ret = io_uring_submit_and_wait(&ring_, wait_nr);
    if (ret > 0) {
        uint32_t submitted = static_cast<uint32_t>(ret);
        pending_sqes_ = (pending_sqes_ >= submitted) ? (pending_sqes_ - submitted) : 0;
    }
    return ret;
}

uint32_t RingEngine::ReapCompletions(std::span<CompletionEvent> out_events) {
    if (!initialized_ || out_events.empty()) return 0;

    uint32_t count = 0;
    struct io_uring_cqe* cqe = nullptr;

    while (count < out_events.size()) {
        int ret = io_uring_peek_cqe(&ring_, &cqe);
        if (ret < 0 || !cqe) {
            break; // No more pending CQEs available right now
        }

        auto* ctx = static_cast<OpContext*>(io_uring_cqe_get_data(cqe));
        if (ctx) {
            ctx->MarkCompleted(cqe->res);
        }

        if (in_flight_ops_ > 0) {
            in_flight_ops_--;
        }

        out_events[count] = CompletionEvent{
            .context = ctx,
            .result = cqe->res,
            .flags = cqe->flags
        };

        count++;
        io_uring_cqe_seen(&ring_, cqe);
    }

    return count;
}

uint32_t RingEngine::ProcessCompletions() {
    CompletionEvent events[64];
    uint32_t total_processed = 0;

    while (true) {
        uint32_t reaped = ReapCompletions(events);
        if (reaped == 0) break;

        for (uint32_t i = 0; i < reaped; ++i) {
            const auto& evt = events[i];
            if (evt.context && evt.context->on_complete) {
                evt.context->on_complete(evt);
            }
        }
        total_processed += reaped;
    }

    return total_processed;
}

void RingEngine::Shutdown() {
    if (initialized_) {
        io_uring_queue_exit(&ring_);
        initialized_ = false;
        pending_sqes_ = 0;
        in_flight_ops_ = 0;
    }
}

} // namespace helios
