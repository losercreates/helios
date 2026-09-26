#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <span>

namespace helios {

// Enum representing low-level Linux io_uring operation types
enum class OpType : uint8_t {
    Nop,
    Read,
    Write,
    Accept,
    Connect,
    Timeout,
    Cancel,
    Shutdown
};

struct OpContext;

// Structured completion event returned upon CQE reaping
struct CompletionEvent {
    OpContext* context{nullptr};
    int32_t result{0};       // CQE res: bytes transferred (>=0) or negative Linux errno (-errno)
    uint32_t flags{0};       // CQE flags
    
    [[nodiscard]] bool IsSuccess() const noexcept { return result >= 0; }
    [[nodiscard]] int GetErrno() const noexcept { return result < 0 ? -result : 0; }
    [[nodiscard]] size_t BytesTransferred() const noexcept { return result > 0 ? static_cast<size_t>(result) : 0; }
};

// Operation Context tracking kernel I/O request state and memory bounds
struct OpContext {
    OpType type{OpType::Nop};
    int fd{-1};
    void* buffer{nullptr};
    size_t bytes_requested{0};
    size_t bytes_transferred{0};
    uint64_t sequence_id{0};
    void* opaque_user_data{nullptr};
    
    // Optional completion callback handler
    std::function<void(const CompletionEvent&)> on_complete;
    
    // In-flight operation lifecycle flag
    bool in_flight{false};

    void MarkInFlight() noexcept { in_flight = true; }
    void MarkCompleted(int32_t res) noexcept { 
        in_flight = false; 
        if (res > 0) {
            bytes_transferred += static_cast<size_t>(res);
        }
    }
};

} // namespace helios
