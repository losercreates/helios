#include <gtest/gtest.h>
#include "net/l4_proxy_server.hpp"
#include "test_backend.hpp"
#include "test_client.hpp"
#include "test_helpers.hpp"
#include <thread>
#include <atomic>

using namespace helios;
using namespace helios::test;

class DataIntegrityTest : public ::testing::Test {
protected:
    void SetUp() override {
        stop_proxy_ = false;
    }
    void TearDown() override {
        StopProxyLoop();
    }

    void StartProxyLoop(L4ProxyServer& proxy) {
        stop_proxy_ = false;
        proxy_thread_ = std::thread([&proxy, this]() {
            while (!stop_proxy_) {
                proxy.RunOnce(std::chrono::milliseconds(1));
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

TEST_F(DataIntegrityTest, LargePayloadRandomBytes) {
    TestBackend backend;
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.buffer_size = 4096;
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    TestClient client;
    ASSERT_TRUE(client.Connect("127.0.0.1", proxy.GetPort()));

    // 256 KB random data
    auto payload = TestHelpers::GenerateRandomPayload(256 * 1024);
    
    std::thread sender([&client, &payload]() {
        client.SendFragmented(std::string(payload.begin(), payload.end()), 16384, std::chrono::milliseconds(0));
    });

    auto received = client.ReceiveExact(payload.size(), std::chrono::seconds(10));
    sender.join();

    EXPECT_EQ(received.size(), payload.size());
    if (received.size() == payload.size()) {
        EXPECT_TRUE(std::equal(received.begin(), received.end(), payload.begin()));
    }

    client.Disconnect();
    backend.Stop();
    StopProxyLoop();
}

TEST_F(DataIntegrityTest, PayloadLargerThanBufferSize) {
    TestBackend backend;
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    config.buffer_size = 1024; // Small buffer
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    TestClient client;
    ASSERT_TRUE(client.Connect("127.0.0.1", proxy.GetPort()));

    // 10KB data, which is 10x larger than buffer size
    auto payload = TestHelpers::GeneratePatternPayload(10 * 1024, {0xDE, 0xAD, 0xBE, 0xEF});
    
    client.Send(payload);
    auto received = client.ReceiveExact(payload.size(), std::chrono::seconds(5));

    EXPECT_EQ(received, payload);

    client.Disconnect();
    backend.Stop();
    StopProxyLoop();
}

TEST_F(DataIntegrityTest, ZeroBytesPayload) {
    TestBackend backend;
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    TestClient client;
    ASSERT_TRUE(client.Connect("127.0.0.1", proxy.GetPort()));

    std::vector<uint8_t> payload(1024, 0); // All zeros
    
    client.Send(payload);
    auto received = client.ReceiveExact(payload.size());

    EXPECT_EQ(received, payload);

    client.Disconnect();
    backend.Stop();
    StopProxyLoop();
}
