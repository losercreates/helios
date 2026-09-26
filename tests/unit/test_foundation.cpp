#include <gtest/gtest.h>
#include "foundation.hpp"
#include <vector>
#include <span>

TEST(FoundationTest, SystemInfoVerification) {
    auto info = helios::GetSystemInfo();
    EXPECT_EQ(info.name, "Helios Core Reverse Proxy & Load Balancer");
    EXPECT_EQ(info.version, "0.1.0");
    EXPECT_GE(info.cpp_standard, 202002L);
}

TEST(FoundationTest, BufferSpanValidation) {
    std::vector<uint8_t> valid_data = {0x01, 0x02, 0x03, 0x04};
    std::vector<uint8_t> empty_data;

    EXPECT_TRUE(helios::ValidateBufferSpan(valid_data));
    EXPECT_FALSE(helios::ValidateBufferSpan(empty_data));
}

TEST(FoundationTest, ConceptCompliance) {
    static_assert(helios::ByteLike<uint8_t>, "uint8_t must satisfy ByteLike concept");
    static_assert(helios::ByteLike<char>, "char must satisfy ByteLike concept");
    static_assert(!helios::ByteLike<uint32_t>, "uint32_t must not satisfy ByteLike concept");
}
