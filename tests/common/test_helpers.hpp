#pragma once

#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <functional>

namespace helios::test {

class TestHelpers {
public:
    static std::vector<uint8_t> GenerateRandomPayload(size_t size);
    static std::vector<uint8_t> GeneratePatternPayload(size_t size, const std::vector<uint8_t>& pattern);
    static bool WaitForCondition(std::function<bool()> condition, std::chrono::milliseconds timeout, std::chrono::milliseconds poll_interval = std::chrono::milliseconds(10));
};

} // namespace helios::test
