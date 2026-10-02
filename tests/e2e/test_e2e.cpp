#include <gtest/gtest.h>
#include "net/l4_proxy_server.hpp"
#include "net/socket_utils.hpp"
#include "test_backend.hpp"
#include "test_client.hpp"
#include "test_helpers.hpp"
#include <thread>
#include <atomic>

using namespace helios;
using namespace helios::test;

class E2EL4ProxyTest : public ::testing::Test {
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
                std::this_thread::sleep_for(std::chrono::microseconds(100));
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

TEST_F(E2EL4ProxyTest, BasicStartAndConnect) {
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

    std::string request = "Hello Helios!";
    ASSERT_GT(client.Send(request), 0);

    std::string response = client.ReceiveString(request.size());
    EXPECT_EQ(response, request);

    client.Disconnect();
    backend.Stop();
    StopProxyLoop();
}

TEST_F(E2EL4ProxyTest, MultipleWritesFromClient) {
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

    std::string msg1 = "Msg1";
    std::string msg2 = "Msg2";
    
    ASSERT_GT(client.Send(msg1), 0);
    EXPECT_EQ(client.ReceiveString(msg1.size()), msg1);

    ASSERT_GT(client.Send(msg2), 0);
    EXPECT_EQ(client.ReceiveString(msg2.size()), msg2);

    client.Disconnect();
    backend.Stop();
    StopProxyLoop();
}

TEST_F(E2EL4ProxyTest, FragmentedClientWrites) {
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

    std::string payload = "FragmentedDataStream";
    client.SendFragmented(payload, 1, std::chrono::milliseconds(5));

    auto received = client.ReceiveExact(payload.size());
    EXPECT_EQ(std::string(received.begin(), received.end()), payload);

    client.Disconnect();
    backend.Stop();
    StopProxyLoop();
}

TEST_F(E2EL4ProxyTest, FragmentedBackendWrites) {
    BackendConfig bconf;
    bconf.behavior = BackendBehavior::SEND_FRAGMENTS;
    bconf.fragment_size = 1;
    bconf.fragment_delay = std::chrono::milliseconds(5);
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

    std::string request = "SendSlowly";
    client.Send(request);

    auto received = client.ReceiveExact(request.size(), std::chrono::seconds(2));
    EXPECT_EQ(std::string(received.begin(), received.end()), request);

    client.Disconnect();
    backend.Stop();
    StopProxyLoop();
}

TEST_F(E2EL4ProxyTest, MultipleSimultaneousConnections) {
    TestBackend backend;
    ASSERT_TRUE(backend.Start());

    L4ProxyServer::Config config;
    config.listen_port = 0;
    L4ProxyServer proxy(config);
    proxy.AddBackend("127.0.0.1", backend.GetPort());
    ASSERT_TRUE(proxy.Start());
    StartProxyLoop(proxy);

    constexpr int NUM_CLIENTS = 10;
    std::vector<TestClient> clients(NUM_CLIENTS);
    for (size_t i = 0; i < NUM_CLIENTS; ++i) {
        ASSERT_TRUE(clients[i].Connect("127.0.0.1", proxy.GetPort()));
    }

    for (size_t i = 0; i < NUM_CLIENTS; ++i) {
        std::string req = "Req" + std::to_string(i);
        clients[i].Send(req);
    }

    for (size_t i = 0; i < NUM_CLIENTS; ++i) {
        std::string req = "Req" + std::to_string(i);
        EXPECT_EQ(clients[i].ReceiveString(req.size()), req);
    }

    for (size_t i = 0; i < NUM_CLIENTS; ++i) {
        clients[i].Disconnect();
    }

    backend.Stop();
    StopProxyLoop();
}
