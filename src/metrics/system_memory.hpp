#pragma once

#include <cstdint>

namespace helios {

struct ProcessMemoryInfo {
    uint64_t resident_bytes{0};
    uint64_t virtual_bytes{0};
};

// Queries real-time process resident (RSS) and virtual (VSIZE) memory sizes in bytes.
// Uses /proc/self/statm on Linux with getrusage() fallback.
ProcessMemoryInfo QueryProcessMemory();

} // namespace helios
