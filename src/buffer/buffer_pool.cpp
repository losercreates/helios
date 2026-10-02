#include "buffer_pool.hpp"
#include <stdexcept>
#include <algorithm>
#include <iostream>

namespace helios {

BufferPool::BufferPool(size_t total_buffers, size_t block_size, double high_watermark, double low_watermark)
    : total_buffers_(total_buffers),
      block_size_(block_size),
      high_watermark_ratio_(high_watermark),
      low_watermark_ratio_(low_watermark) {
    if (total_buffers_ == 0 || block_size_ == 0) {
        throw std::invalid_argument("BufferPool total_buffers and block_size must be positive");
    }
    if (high_watermark_ratio_ <= 0.0 || high_watermark_ratio_ > 1.0) {
        throw std::invalid_argument("high_watermark must be in range (0.0, 1.0]");
    }
    if (low_watermark_ratio_ <= 0.0 || low_watermark_ratio_ >= high_watermark_ratio_) {
        throw std::invalid_argument("low_watermark must be in range (0.0, high_watermark)");
    }

    contiguous_memory_block_.resize(total_buffers_ * block_size_, 0);
    buffers_.resize(total_buffers_);
    free_stack_.reserve(total_buffers_);

    for (size_t i = 0; i < total_buffers_; ++i) {
        buffers_[i].buffer_id = static_cast<uint32_t>(i);
        buffers_[i].data = contiguous_memory_block_.data() + (i * block_size_);
        buffers_[i].capacity = block_size_;
        buffers_[i].Reset();
        buffers_[i].is_checked_out = false;

        free_stack_.push_back(static_cast<uint32_t>(total_buffers_ - 1 - i));
    }
}

Buffer* BufferPool::Acquire() {
    if (free_stack_.empty()) {
        std::cerr << "BUFFER POOL EXHAUSTED!\n";
        UpdateBackpressureState();
        return nullptr;
    }

    uint32_t idx = free_stack_.back();
    free_stack_.pop_back();

    Buffer* buf = &buffers_[idx];
    buf->Reset();
    buf->ref_count = 1;
    buf->is_checked_out = true;
    checked_out_count_++;

    UpdateBackpressureState();
    return buf;
}

void BufferPool::Retain(Buffer* buf) {
    if (!buf || !buf->is_checked_out) return;
    buf->ref_count++;
}

bool BufferPool::Release(Buffer* buf) {
    if (!buf) return false;

    // Double-release / use-after-release protection
    if (!buf->is_checked_out || buf->ref_count == 0) {
        std::cerr << "RELEASE FAILED! checked_out=" << buf->is_checked_out << " ref=" << buf->ref_count << "\n";
        return false;
    }

    buf->ref_count--;
    if (buf->ref_count == 0) {
        buf->is_checked_out = false;
        buf->Reset();
        free_stack_.push_back(buf->buffer_id);
        if (checked_out_count_ > 0) {
            checked_out_count_--;
        }
        UpdateBackpressureState();
        return true;
    }

    return false;
}

double BufferPool::UtilizationRatio() const noexcept {
    if (total_buffers_ == 0) return 0.0;
    return static_cast<double>(checked_out_count_) / static_cast<double>(total_buffers_);
}

bool BufferPool::IsHighWatermarkReached() const noexcept {
    return UtilizationRatio() >= high_watermark_ratio_;
}

bool BufferPool::IsLowWatermarkResumed() const noexcept {
    return UtilizationRatio() <= low_watermark_ratio_;
}

bool BufferPool::UpdateBackpressureState() {
    double ratio = UtilizationRatio();
    if (!backpressure_active_ && ratio >= high_watermark_ratio_) {
        backpressure_active_ = true;
    } else if (backpressure_active_ && ratio <= low_watermark_ratio_) {
        backpressure_active_ = false;
    }
    return backpressure_active_;
}

} // namespace helios
