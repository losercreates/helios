#include "foundation.hpp"
#include "net/l4_proxy_server.hpp"
#include <iostream>

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    auto info = helios::GetSystemInfo();

    std::cout << "[HELIOS] Starting " << info.name << " v" << info.version << "\n";
    std::cout << "[HELIOS] Compiled with C++ standard: " << info.cpp_standard << "\n";

    helios::L4ProxyServer::Config config;
    config.listen_port = 8080;
    helios::L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", 9001);

    if (proxy.Start()) {
        std::cout << "[HELIOS] Single-Threaded L4 TCP Reverse Proxy initialized on port " << proxy.GetPort() << "\n";
        proxy.Stop();
    } else {
        std::cout << "[HELIOS] Listener initialization on port " << config.listen_port << " (already in use or permissions restricted)\n";
    }

    std::cout << "[HELIOS] Exiting cleanly (0).\n";

    return 0;
}
