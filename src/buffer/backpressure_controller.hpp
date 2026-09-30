#pragma once

#include "buffer_pool.hpp"

namespace helios {

// Controller managing read and accept pause/resume state based on BufferPool watermarks
class BackpressureController {
public:
    explicit BackpressureController(BufferPool& pool) noexcept
        : pool_(pool) {}

    // Evaluate pool watermarks and update backpressure signals
    void Evaluate() noexcept {
        bool was_engaged = backpressure_engaged_;
        bool is_high = pool_.IsHighWatermarkReached();
        bool is_low = pool_.IsLowWatermarkResumed();

        if (!was_engaged && is_high) {
            backpressure_engaged_ = true;
            reads_paused_ = true;
            accepts_paused_ = true;
        } else if (was_engaged && is_low) {
            backpressure_engaged_ = false;
            reads_paused_ = false;
            accepts_paused_ = false;
        }
    }

    [[nodiscard]] bool IsEngaged() const noexcept { return backpressure_engaged_; }
    [[nodiscard]] bool AreReadsPaused() const noexcept { return reads_paused_; }
    [[nodiscard]] bool AreAcceptsPaused() const noexcept { return accepts_paused_; }

private:
    BufferPool& pool_;
    bool backpressure_engaged_{false};
    bool reads_paused_{false};
    bool accepts_paused_{false};
};

} // namespace helios
