#pragma once

#include "op_context.hpp"
#include <liburing.h>
#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
#include <sys/socket.h>

namespace helios {

// Thin C++20 abstraction wrapper over Linux liburing kernel submission and completion queues
class RingEngine {
public:
    explicit RingEngine(uint32_t entries = 1024, uint32_t flags = 0);
    ~RingEngine();

    // Prevent copying (strict single-ring ownership model)
    RingEngine(const RingEngine&) = delete;
    RingEngine& operator=(const RingEngine&) = delete;

    // Move semantics
    RingEngine(RingEngine&& other) noexcept;
    RingEngine& operator=(RingEngine&& other) noexcept;

    // Status queries
    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }
    [[nodiscard]] uint32_t Capacity() const noexcept { return capacity_; }
    [[nodiscard]] uint32_t PendingSQEs() const noexcept { return pending_sqes_; }
    [[nodiscard]] uint32_t InFlightOps() const noexcept { return in_flight_ops_; }

    // Operation Submission Preparation
    bool PrepNop(OpContext* ctx);
    bool PrepRead(int fd, void* buf, size_t nbytes, uint64_t offset, OpContext* ctx);
    bool PrepWrite(int fd, const void* buf, size_t nbytes, uint64_t offset, OpContext* ctx);
    bool PrepAccept(int listener_fd, sockaddr* addr, socklen_t* addrlen, int flags, OpContext* ctx);
    bool PrepConnect(int fd, const sockaddr* addr, socklen_t addrlen, OpContext* ctx);
    bool PrepCancel(OpContext* target_ctx, OpContext* cancel_ctx);

    // Queue Submission
    int Submit();
    int SubmitAndWait(uint32_t wait_nr);

    // Completion Event Reaping
    uint32_t ReapCompletions(std::span<CompletionEvent> out_events);
    uint32_t ProcessCompletions(); // Reaps completions and invokes ctx->on_complete callback if present

    // Teardown
    void Shutdown();

private:
    struct io_uring ring_{};
    uint32_t capacity_{0};
    uint32_t pending_sqes_{0};
    uint32_t in_flight_ops_{0};
    bool initialized_{false};

    struct io_uring_sqe* GetSqeOrNull() noexcept;
    void TagSqe(struct io_uring_sqe* sqe, OpContext* ctx) noexcept;
};

} // namespace helios
