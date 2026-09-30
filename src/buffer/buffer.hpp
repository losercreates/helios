#pragma once

#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <span>

namespace helios {

// Fixed-capacity memory buffer descriptor pointing into a pre-allocated contiguous memory block
struct Buffer {
    uint32_t buffer_id{0};
    uint8_t* data{nullptr};
    size_t capacity{16384};       // Default: 16 KB
    size_t read_offset{0};
    size_t write_offset{0};
    uint32_t ref_count{0};
    bool is_checked_out{false};

    [[nodiscard]] size_t ReadableBytes() const noexcept {
        return (write_offset >= read_offset) ? (write_offset - read_offset) : 0;
    }

    [[nodiscard]] size_t WritableBytes() const noexcept {
        return (capacity >= write_offset) ? (capacity - write_offset) : 0;
    }

    [[nodiscard]] uint8_t* ReadPtr() noexcept {
        return data + read_offset;
    }

    [[nodiscard]] const uint8_t* ReadPtr() const noexcept {
        return data + read_offset;
    }

    [[nodiscard]] uint8_t* WritePtr() noexcept {
        return data + write_offset;
    }

    [[nodiscard]] const uint8_t* WritePtr() const noexcept {
        return data + write_offset;
    }

    [[nodiscard]] std::span<uint8_t> ReadableSpan() noexcept {
        return std::span<uint8_t>(ReadPtr(), ReadableBytes());
    }

    [[nodiscard]] std::span<uint8_t> WritableSpan() noexcept {
        return std::span<uint8_t>(WritePtr(), WritableBytes());
    }

    void AdvanceRead(size_t bytes) noexcept {
        read_offset = std::min(read_offset + bytes, write_offset);
    }

    void AdvanceWrite(size_t bytes) noexcept {
        write_offset = std::min(write_offset + bytes, capacity);
    }

    void Reset() noexcept {
        read_offset = 0;
        write_offset = 0;
        ref_count = 0;
    }
};

} // namespace helios
