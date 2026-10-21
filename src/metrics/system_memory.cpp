#include "metrics/system_memory.hpp"

#include <unistd.h>
#include <sys/resource.h>
#include <fstream>
#include <sstream>
#include <string>

namespace helios {

ProcessMemoryInfo QueryProcessMemory() {
    ProcessMemoryInfo info{};
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        page_size = 4096;
    }

    // Try reading /proc/self/statm (Linux specific, fast, precise)
    // Format: size resident shared text lib data dt (all in page units)
    std::ifstream statm("/proc/self/statm");
    if (statm.is_open()) {
        uint64_t v_pages = 0;
        uint64_t r_pages = 0;
        if (statm >> v_pages >> r_pages) {
            info.virtual_bytes = v_pages * static_cast<uint64_t>(page_size);
            info.resident_bytes = r_pages * static_cast<uint64_t>(page_size);
            return info;
        }
    }

    // Fallback: getrusage(RUSAGE_SELF, ...)
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        // On Linux, ru_maxrss is in kilobytes
        info.resident_bytes = static_cast<uint64_t>(usage.ru_maxrss) * 1024ULL;
        if (info.virtual_bytes == 0) {
            info.virtual_bytes = info.resident_bytes;
        }
    }

    return info;
}

} // namespace helios
