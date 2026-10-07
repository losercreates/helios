#include <gtest/gtest.h>
#include "lb/health_checker.hpp"
#include "io/ring_engine.hpp"
#include "../common/test_tcp_backend.hpp"
#include <vector>
#include <memory>
#include <chrono>

using namespace helios;
using namespace std::chrono_literals;

TEST(HealthCheckerStressTest, MultipleBackendsConcurrentChecks) {
    RingEngine engine(1024);
    
    std::vector<std::unique_ptr<TestTcpBackend>> servers;
    std::vector<std::unique_ptr<HealthChecker>> checkers;
    std::vector<std::shared_ptr<Backend>> backends;

    int num_backends = 100;
    
    for (int i = 0; i < num_backends; ++i) {
        auto server = std::make_unique<TestTcpBackend>();
        server->Start(); // Note: some might fail to start if out of ports, but assuming local test is fine
        uint16_t port = server->GetPort();
        servers.push_back(std::move(server));
        
        auto backend = std::make_shared<Backend>(i + 1, "127.0.0.1", port);
        backend->SetHealthThresholds(2, 2);
        backends.push_back(backend);
        
        auto checker = std::make_unique<HealthChecker>(engine, backend, 10ms, 10ms);
        checker->Start();
        checkers.push_back(std::move(checker));
    }
    
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 500ms) {
        engine.ProcessCompletions();
        engine.Submit();
        
        // Randomly stop and start some servers
        for (int i = 0; i < 5; ++i) {
            size_t idx = static_cast<size_t>(rand() % num_backends);
            if (servers[idx]->IsRunning()) {
                servers[idx]->Stop();
            } else {
                servers[idx]->Start();
            }
        }
        
        std::this_thread::sleep_for(2ms);
    }
    
    // Stop all checkers to clean up
    for (auto& checker : checkers) {
        checker->Stop();
    }
    
    // Reap cancellations
    start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 100ms) {
        engine.ProcessCompletions();
        engine.Submit();
    }
    
    EXPECT_EQ(engine.InFlightOps(), 0u);
}
