#include <gtest/gtest.h>
#include <cstdlib>
#include <string>

TEST(FoundationIntegrationTest, MainExecutableStartupAndCleanExit) {
    // Path to built executable passed via compile definition or relative run path
#ifdef HELIOS_EXECUTABLE_PATH
    std::string command = HELIOS_EXECUTABLE_PATH;
#else
    std::string command = "./bin/helios";
#endif

    int result = std::system(command.c_str());
    EXPECT_EQ(result, 0) << "Main executable failed to run or exited with non-zero status.";
}
