#include "foundation.hpp"

namespace helios {

SystemInfo GetSystemInfo() noexcept {
    return SystemInfo{
        .name = kProjectName,
        .version = kProjectVersion,
        .cpp_standard = static_cast<uint32_t>(__cplusplus)
    };
}

bool ValidateBufferSpan(std::span<const uint8_t> buffer) noexcept {
    return !buffer.empty();
}

} // namespace helios
