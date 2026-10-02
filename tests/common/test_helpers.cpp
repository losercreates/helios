#include "test_helpers.hpp"
#include <random>
#include <algorithm>

namespace helios::test {

std::vector<uint8_t> TestHelpers::GenerateRandomPayload(size_t size) {
    std::vector<uint8_t> payload(size);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, 255);
    for (size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<uint8_t>(distrib(gen));
    }
    return payload;
}

std::vector<uint8_t> TestHelpers::GeneratePatternPayload(size_t size, const std::vector<uint8_t>& pattern) {
    if (pattern.empty()) return std::vector<uint8_t>(size, 0);
    std::vector<uint8_t> payload(size);
    for (size_t i = 0; i < size; ++i) {
        payload[i] = pattern[i % pattern.size()];
    }
    return payload;
}

bool TestHelpers::WaitForCondition(std::function<bool()> condition, std::chrono::milliseconds timeout, std::chrono::milliseconds poll_interval) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (condition()) return true;
        auto now = std::chrono::steady_clock::now();
        if (now - start >= timeout) return false;
        std::this_thread::sleep_for(poll_interval);
    }
}

} // namespace helios::test
