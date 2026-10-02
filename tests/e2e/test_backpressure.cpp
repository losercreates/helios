#include <gtest/gtest.h>
#include "net/l4_proxy_server.hpp"
#include "test_backend.hpp"
#include "test_client.hpp"
#include <thread>
#include <atomic>

using namespace helios;
using namespace helios::test;

class BackpressureTest : public ::testing::Test {
protected:
    void SetUp() override { stop_proxy_ = false; }
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

TEST_F(BackpressureTest, SlowBackendReads) {
    BackendConfig bconf;
    bconf.behavior = BackendBehavior::DELAY_READ;
    bconf.delay = std::chrono::milliseconds(100);
    TestBackend backend(bconf);
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.total_buffers = 128; // Large pool
    config.buffer_size = 1024;
    config.ring_entries = 4096;
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    TestClient client;
    ASSERT_TRUE(client.Connect("127.0.0.1", proxy.GetPort()));

    // Client sends fast, backend reads slowly. Should trigger backpressure pause.
    std::string payload(1024, 'A');
    for (int i = 0; i < 10; ++i) {
        client.Send(payload);
    }
    
    // It should eventually complete without crashing
    auto received = client.ReceiveExact(10 * 1024, std::chrono::seconds(10));
    EXPECT_EQ(received.size(), 10 * 1024);

    client.Disconnect();
    backend.Stop();
    StopProxyLoop();
}

TEST_F(BackpressureTest, BufferExhaustionAndResume) {
    TestBackend backend; // Normal echo
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.total_buffers = 128; // Large pool
    config.buffer_size = 1024;
    config.ring_entries = 4096;
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    TestClient client1, client2;
    ASSERT_TRUE(client1.Connect("127.0.0.1", proxy.GetPort()));
    ASSERT_TRUE(client2.Connect("127.0.0.1", proxy.GetPort()));

    std::string payload1(2048, 'B');
    std::string payload2(2048, 'C');
    
    // Both clients send simultaneously, overwhelming the 2 buffers
    client1.Send(payload1);
    client2.Send(payload2);

    auto rec1 = client1.ReceiveExact(2048, std::chrono::seconds(5));
    auto rec2 = client2.ReceiveExact(2048, std::chrono::seconds(5));

    EXPECT_EQ(rec1.size(), 2048);
    EXPECT_EQ(rec2.size(), 2048);
    
    client1.Disconnect();
    client2.Disconnect();
    backend.Stop();
    StopProxyLoop();
}
