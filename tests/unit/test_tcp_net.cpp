#include <gtest/gtest.h>
#include "net/socket_utils.hpp"
#include "net/tcp_listener.hpp"
#include "net/tcp_connection.hpp"
#include "io/ring_engine.hpp"

using namespace helios;

TEST(TcpNetUnitTest, SocketUtilsAddressParsing) {
    sockaddr_in addr{};
    EXPECT_TRUE(SocketUtils::ParseSockAddr("127.0.0.1", 8080, &addr));
    EXPECT_EQ(ntohs(addr.sin_port), 8080);
    EXPECT_EQ(SocketUtils::FormatEndpoint(addr), "127.0.0.1:8080");

    EXPECT_TRUE(SocketUtils::ParseSockAddr("0.0.0.0", 9000, &addr));
    EXPECT_EQ(ntohs(addr.sin_port), 9000);

    EXPECT_FALSE(SocketUtils::ParseSockAddr("invalid_ip_address", 80, &addr));
}

TEST(TcpNetUnitTest, ListenerLifecycle) {
    RingEngine engine(64);
    TcpListener listener(engine);

    EXPECT_FALSE(listener.IsListening());
    EXPECT_EQ(listener.GetFd(), -1);

    // Bind to loopback port 0 (dynamic port allocation)
    ASSERT_TRUE(listener.BindAndListen("127.0.0.1", 0));
    EXPECT_TRUE(listener.IsListening());
    EXPECT_GT(listener.GetFd(), 0);
    EXPECT_GT(listener.GetPort(), 0u);

    listener.Close();
    EXPECT_FALSE(listener.IsListening());
}

TEST(TcpNetUnitTest, ListenerMoveSemantics) {
    RingEngine engine(64);
    TcpListener listener1(engine);
    ASSERT_TRUE(listener1.BindAndListen("127.0.0.1", 0));
    uint16_t port = listener1.GetPort();
    int fd = listener1.GetFd();

    TcpListener listener2(std::move(listener1));
    EXPECT_FALSE(listener1.IsListening());
    EXPECT_TRUE(listener2.IsListening());
    EXPECT_EQ(listener2.GetPort(), port);
    EXPECT_EQ(listener2.GetFd(), fd);
}

TEST(TcpNetUnitTest, ConnectionStateAndMove) {
    RingEngine engine(64);
    TcpConnection conn1(engine);

    EXPECT_EQ(conn1.GetState(), SocketState::Disconnected);
    EXPECT_FALSE(conn1.IsOpen());

    TcpConnection conn2(engine, 100);
    EXPECT_TRUE(conn2.IsOpen());
    EXPECT_EQ(conn2.GetState(), SocketState::Connected);

    TcpConnection conn3(std::move(conn2));
    EXPECT_FALSE(conn2.IsOpen());
    EXPECT_TRUE(conn3.IsOpen());
    EXPECT_EQ(conn3.GetFd(), 100);
    
    conn3.Close();
    EXPECT_FALSE(conn3.IsOpen());
    EXPECT_EQ(conn3.GetState(), SocketState::Closed);
}
