#pragma once

#include "buffer.hpp"
#include <vector>
#include <cstdint>
#include <cstddef>
#include <atomic>

namespace helios {

// Fixed-capacity zero-heap-allocation BufferPool with watermark backpressure tracking
class BufferPool {
public:
    BufferPool(size_t total_buffers = 1024, size_t block_size = 16384,
               double high_watermark = 1.0, double low_watermark = 0.8);
    ~BufferPool() = default;

    // Non-copyable
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // Moveable
    BufferPool(BufferPool&& other) noexcept
        : total_buffers_(other.total_buffers_),
          block_size_(other.block_size_),
          high_watermark_ratio_(other.high_watermark_ratio_),
          low_watermark_ratio_(other.low_watermark_ratio_),
          contiguous_memory_block_(std::move(other.contiguous_memory_block_)),
          buffers_(std::move(other.buffers_)),
          free_stack_(std::move(other.free_stack_)),
          checked_out_count_(other.checked_out_count_.load(std::memory_order_relaxed)),
          backpressure_active_(other.backpressure_active_) {}
    BufferPool& operator=(BufferPool&&) = delete;

    // Core Buffer Allocator API
    Buffer* Acquire();
    void Retain(Buffer* buf);
    bool Release(Buffer* buf); // Returns true if buffer was returned to free stack

    // Pool Status Queries
    [[nodiscard]] size_t TotalBuffers() const noexcept { return total_buffers_; }
    [[nodiscard]] size_t BlockSize() const noexcept { return block_size_; }
    [[nodiscard]] size_t CheckedOutCount() const noexcept { return checked_out_count_.load(std::memory_order_relaxed); }
    [[nodiscard]] size_t FreeCount() const noexcept { return free_stack_.size(); }
    [[nodiscard]] size_t TotalAllocatedBytes() const noexcept { return total_buffers_ * block_size_; }
    [[nodiscard]] size_t ActiveAllocatedBytes() const noexcept { return checked_out_count_.load(std::memory_order_relaxed) * block_size_; }
    [[nodiscard]] double UtilizationRatio() const noexcept;
    [[nodiscard]] bool IsExhausted() const noexcept { return free_stack_.empty(); }

    // Watermark & Backpressure Queries
    [[nodiscard]] double HighWatermarkRatio() const noexcept { return high_watermark_ratio_; }
    [[nodiscard]] double LowWatermarkRatio() const noexcept { return low_watermark_ratio_; }
    [[nodiscard]] bool IsHighWatermarkReached() const noexcept;
    [[nodiscard]] bool IsLowWatermarkResumed() const noexcept;
    [[nodiscard]] bool IsBackpressureActive() const noexcept { return backpressure_active_; }

    // Evaluate watermarks and update backpressure state
    bool UpdateBackpressureState();

private:
    size_t total_buffers_{0};
    size_t block_size_{0};
    double high_watermark_ratio_{1.0};
    double low_watermark_ratio_{0.8};

    std::vector<uint8_t> contiguous_memory_block_;
    std::vector<Buffer> buffers_;
    std::vector<uint32_t> free_stack_;
    std::atomic<size_t> checked_out_count_{0};
    bool backpressure_active_{false};
};

} // namespace helios
