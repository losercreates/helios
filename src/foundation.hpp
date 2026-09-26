#pragma once

#include <string_view>
#include <concepts>
#include <span>
#include <cstdint>

namespace helios {

// Core project identity metadata
constexpr std::string_view kProjectName = "Helios Core Reverse Proxy & Load Balancer";
constexpr std::string_view kProjectVersion = "0.1.0";

// C++20 Concept check for byte buffers
template <typename T>
concept ByteLike = sizeof(T) == 1;

// Foundation info structure
struct SystemInfo {
    std::string_view name;
    std::string_view version;
    uint32_t cpp_standard;
};

// Function returning basic system info
SystemInfo GetSystemInfo() noexcept;

// Helper to verify C++20 std::span functionality
bool ValidateBufferSpan(std::span<const uint8_t> buffer) noexcept;

} // namespace helios
