#include <gtest/gtest.h>
#include "net/l4_proxy_server.hpp"
#include "test_backend.hpp"
#include "test_client.hpp"
#include <thread>
#include <atomic>

using namespace helios;
using namespace helios::test;

class ConnectionLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override { stop_proxy_ = false; }
    void TearDown() override { StopProxyLoop(); }
    void StartProxyLoop(L4ProxyServer& proxy) {
        stop_proxy_ = false;
        proxy_thread_ = std::thread([&proxy, this]() {
            while (!stop_proxy_) { proxy.RunOnce(std::chrono::milliseconds(1)); }
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

TEST_F(ConnectionLifecycleTest, NormalClose) {
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
    client.Disconnect(); // client initiates close

    // Wait for proxy to clean up
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(proxy.GetActiveConnectionCount(), 0);

    backend.Stop();
    StopProxyLoop();
}

TEST_F(ConnectionLifecycleTest, ClientHalfClose) {
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
    
    std::string request = "HalfCloseRequest";
    client.Send(request);
    client.HalfClose(); // Send FIN

    auto received = client.ReceiveExact(request.size());
    EXPECT_EQ(std::string(received.begin(), received.end()), request);

    client.Disconnect();
    backend.Stop();
    StopProxyLoop();
}

TEST_F(ConnectionLifecycleTest, BackendHalfClose) {
    BackendConfig bconf;
    bconf.behavior = BackendBehavior::HALF_CLOSE_AFTER_READ;
    TestBackend backend(bconf);
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    TestClient client;
    ASSERT_TRUE(client.Connect("127.0.0.1", proxy.GetPort()));
    
    std::string request = "TriggerHalfClose";
    client.Send(request);
    
    auto received = client.ReceiveUntilClose();
    // Expect 0 bytes back since backend half-closed immediately after reading
    EXPECT_EQ(received.size(), 0);

    client.Disconnect();
    backend.Stop();
    StopProxyLoop();
}
