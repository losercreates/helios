#include "foundation.hpp"
#include <iostream>

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    auto info = helios::GetSystemInfo();

    std::cout << "[HELIOS] Starting " << info.name << " v" << info.version << "\n";
    std::cout << "[HELIOS] Compiled with C++ standard: " << info.cpp_standard << "\n";
    std::cout << "[HELIOS] Repository & Build/Test Foundation Initialized Cleanly.\n";
    std::cout << "[HELIOS] Exiting cleanly (0).\n";

    return 0;
}
