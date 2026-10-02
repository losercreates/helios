#include <gtest/gtest.h>
#include "net/l4_proxy_server.hpp"
#include "test_backend.hpp"
#include "test_client.hpp"
#include <thread>
#include <atomic>
#include <vector>
#include <signal.h>

using namespace helios;
using namespace helios::test;

class StressTest : public ::testing::Test {
protected:
    void SetUp() override {
        signal(SIGPIPE, SIG_IGN);
        stop_proxy_ = false;
    }
    void TearDown() override { StopProxyLoop(); }
    void StartProxyLoop(L4ProxyServer& proxy) {
        stop_proxy_ = false;
        proxy_thread_ = std::thread([&proxy, this]() {
            while (!stop_proxy_) {
                proxy.RunOnce(std::chrono::milliseconds(1));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            proxy.Stop();
        });
    }
    void StopProxyLoop() {
        if (proxy_thread_.joinable()) {
            stop_proxy_ = true;
            proxy_thread_.join();
        }
    }
    std::atomic<bool> stop_proxy_{false};
    std::thread proxy_thread_;
};

// Parameterized options could be implemented using environment variables or command-line args,
// but for standard CTest integration we define a baseline stress test.
TEST_F(StressTest, ConnectionStorm) {
    TestBackend backend;
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.buffer_size = 4096;
    config.total_buffers = 1000;
    config.ring_entries = 4096;
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    constexpr int NUM_CONNECTIONS = 100;
    constexpr int MESSAGES_PER_CONN = 100;
    
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    auto start_time = std::chrono::steady_clock::now();

    for (int i = 0; i < NUM_CONNECTIONS; ++i) {
        threads.emplace_back([&, i]() {
            TestClient client;
            if (!client.Connect("127.0.0.1", proxy.GetPort())) return;
            
            std::string payload = "StressPayload" + std::to_string(i);
            
            for (int m = 0; m < MESSAGES_PER_CONN; ++m) {
                if (client.Send(payload) <= 0) return;
                auto received = client.ReceiveExact(payload.size(), std::chrono::milliseconds(30000));
                std::string received_str(received.begin(), received.end());
                if (received_str != payload) {
                    std::cerr << "Client " << i << " expected " << payload << " got " << received_str << " on message " << m << "\n";
                    return;
                }
            }
            client.Disconnect();
            success_count++;
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto duration = std::chrono::steady_clock::now() - start_time;
    
    std::cout << "[ STRESS TEST REPORT ]\n";
    std::cout << "Target connections: " << NUM_CONNECTIONS << "\n";
    std::cout << "Messages per connection: " << MESSAGES_PER_CONN << "\n";
    std::cout << "Successful connections: " << success_count.load() << "\n";
    std::cout << "Test duration: " << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() << " ms\n";
    
    EXPECT_EQ(success_count.load(), NUM_CONNECTIONS);

    // Wait for clean up
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    backend.Stop();
    StopProxyLoop();

    // Now that the proxy thread is joined, safely read the connection count
    EXPECT_EQ(proxy.GetActiveConnectionCount(), 0);
}
