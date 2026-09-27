#include <gtest/gtest.h>
#include "io/ring_engine.hpp"
#include "io/op_context.hpp"
#include "net/socket_utils.hpp"
#include "net/tcp_listener.hpp"
#include "net/tcp_connection.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <array>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

using namespace helios;

class TcpNetIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// 1. Successful Accept & Connection
TEST_F(TcpNetIntegrationTest, AsyncAcceptAndConnectSuccess) {
    RingEngine engine(128);
    TcpListener listener(engine);
    ASSERT_TRUE(listener.BindAndListen("127.0.0.1", 0));
    uint16_t port = listener.GetPort();

    sockaddr_in client_addr{};
    socklen_t client_addr_len = sizeof(client_addr);
    OpContext accept_ctx;
    ASSERT_TRUE(listener.AsyncAccept(&client_addr, &client_addr_len, &accept_ctx));

    sockaddr_in target_addr{};
    OpContext connect_ctx;
    TcpConnection client_conn(engine);
    ASSERT_TRUE(client_conn.AsyncConnect("127.0.0.1", port, &target_addr, &connect_ctx));

    // Submit both accept and connect SQEs to io_uring
    int submitted = engine.SubmitAndWait(2);
    EXPECT_GE(submitted, 2);

    std::array<CompletionEvent, 8> events{};
    uint32_t reaped = engine.ReapCompletions(events);
    EXPECT_GE(reaped, 2u);

    // Verify both accept and connect completed successfully
    bool accept_ok = false;
    bool connect_ok = false;
    int accepted_fd = -1;

    for (uint32_t i = 0; i < reaped; ++i) {
        if (events[i].context == &accept_ctx) {
            EXPECT_TRUE(events[i].IsSuccess());
            EXPECT_GE(events[i].result, 0);
            accepted_fd = events[i].result;
            accept_ok = true;
        } else if (events[i].context == &connect_ctx) {
            EXPECT_TRUE(events[i].IsSuccess());
            EXPECT_EQ(events[i].result, 0);
            connect_ok = true;
        }
    }

    EXPECT_TRUE(accept_ok);
    EXPECT_TRUE(connect_ok);
    EXPECT_GT(accepted_fd, 0);

    if (accepted_fd >= 0) {
        close(accepted_fd);
    }
}

// 2. Data Transfer (Write & Read through io_uring)
TEST_F(TcpNetIntegrationTest, BiDirectionalDataTransfer) {
    RingEngine engine(128);
    TcpListener listener(engine);
    ASSERT_TRUE(listener.BindAndListen("127.0.0.1", 0));

    sockaddr_in client_addr{};
    socklen_t client_addr_len = sizeof(client_addr);
    OpContext accept_ctx;
    ASSERT_TRUE(listener.AsyncAccept(&client_addr, &client_addr_len, &accept_ctx));

    sockaddr_in target_addr{};
    OpContext connect_ctx;
    TcpConnection client_conn(engine);
    ASSERT_TRUE(client_conn.AsyncConnect("127.0.0.1", listener.GetPort(), &target_addr, &connect_ctx));

    engine.SubmitAndWait(2);
    std::array<CompletionEvent, 8> events{};
    uint32_t reaped = engine.ReapCompletions(events);
    ASSERT_GE(reaped, 2u);

    int accepted_fd = -1;
    for (uint32_t i = 0; i < reaped; ++i) {
        if (events[i].context == &accept_ctx) accepted_fd = events[i].result;
    }
    ASSERT_GT(accepted_fd, 0);
    TcpConnection server_conn(engine, accepted_fd);

    // Write from client to server
    const std::string client_msg = "Hello from Client via io_uring!";
    OpContext client_write_ctx;
    ASSERT_TRUE(client_conn.AsyncWrite(client_msg.data(), client_msg.size(), &client_write_ctx));
    engine.SubmitAndWait(1);
    engine.ReapCompletions(events);

    // Read on server
    std::array<char, 128> server_buf{};
    OpContext server_read_ctx;
    ASSERT_TRUE(server_conn.AsyncRead(server_buf.data(), server_buf.size(), &server_read_ctx));
    engine.SubmitAndWait(1);
    reaped = engine.ReapCompletions(events);
    ASSERT_EQ(reaped, 1u);
    EXPECT_EQ(events[0].BytesTransferred(), client_msg.size());
    EXPECT_EQ(std::string_view(server_buf.data(), events[0].BytesTransferred()), client_msg);
}

// 3. Partial Read Behavior
TEST_F(TcpNetIntegrationTest, PartialReadSlicing) {
    RingEngine engine(128);
    TcpListener listener(engine);
    ASSERT_TRUE(listener.BindAndListen("127.0.0.1", 0));

    sockaddr_in client_addr{};
    socklen_t client_addr_len = sizeof(client_addr);
    OpContext accept_ctx;
    ASSERT_TRUE(listener.AsyncAccept(&client_addr, &client_addr_len, &accept_ctx));

    sockaddr_in target_addr{};
    OpContext connect_ctx;
    TcpConnection client_conn(engine);
    ASSERT_TRUE(client_conn.AsyncConnect("127.0.0.1", listener.GetPort(), &target_addr, &connect_ctx));

    engine.SubmitAndWait(2);
    std::array<CompletionEvent, 8> events{};
    uint32_t reaped = engine.ReapCompletions(events);
    ASSERT_GE(reaped, 2u);

    int accepted_fd = -1;
    for (uint32_t i = 0; i < reaped; ++i) {
        if (events[i].context == &accept_ctx) accepted_fd = events[i].result;
    }
    ASSERT_GT(accepted_fd, 0);
    TcpConnection server_conn(engine, accepted_fd);

    const std::string full_payload = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    OpContext write_ctx;
    ASSERT_TRUE(client_conn.AsyncWrite(full_payload.data(), full_payload.size(), &write_ctx));
    engine.SubmitAndWait(1);
    engine.ReapCompletions(events);

    // First read: slice of 10 bytes
    std::array<char, 10> slice1{};
    OpContext read1_ctx;
    ASSERT_TRUE(server_conn.AsyncRead(slice1.data(), slice1.size(), &read1_ctx));
    engine.SubmitAndWait(1);
    reaped = engine.ReapCompletions(events);
    ASSERT_EQ(reaped, 1u);
    EXPECT_EQ(events[0].BytesTransferred(), 10u);
    EXPECT_EQ(std::string_view(slice1.data(), 10), full_payload.substr(0, 10));

    // Second read: slice of remaining bytes
    std::array<char, 64> slice2{};
    OpContext read2_ctx;
    ASSERT_TRUE(server_conn.AsyncRead(slice2.data(), slice2.size(), &read2_ctx));
    engine.SubmitAndWait(1);
    reaped = engine.ReapCompletions(events);
    ASSERT_EQ(reaped, 1u);
    EXPECT_EQ(events[0].BytesTransferred(), full_payload.size() - 10);
    EXPECT_EQ(std::string_view(slice2.data(), events[0].BytesTransferred()), full_payload.substr(10));
}

// 4. Partial Write Slicing
TEST_F(TcpNetIntegrationTest, PartialWriteOffsetTracking) {
    RingEngine engine(128);
    TcpListener listener(engine);
    ASSERT_TRUE(listener.BindAndListen("127.0.0.1", 0));

    sockaddr_in client_addr{};
    socklen_t client_addr_len = sizeof(client_addr);
    OpContext accept_ctx;
    ASSERT_TRUE(listener.AsyncAccept(&client_addr, &client_addr_len, &accept_ctx));

    sockaddr_in target_addr{};
    OpContext connect_ctx;
    TcpConnection client_conn(engine);
    ASSERT_TRUE(client_conn.AsyncConnect("127.0.0.1", listener.GetPort(), &target_addr, &connect_ctx));

    engine.SubmitAndWait(2);
    std::array<CompletionEvent, 8> events{};
    uint32_t reaped = engine.ReapCompletions(events);
    ASSERT_GE(reaped, 2u);

    int accepted_fd = -1;
    for (uint32_t i = 0; i < reaped; ++i) {
        if (events[i].context == &accept_ctx) accepted_fd = events[i].result;
    }
    ASSERT_GT(accepted_fd, 0);
    TcpConnection server_conn(engine, accepted_fd);

    const std::string chunk1 = "ChunkOne_";
    const std::string chunk2 = "ChunkTwo";

    OpContext w1_ctx, w2_ctx;
    ASSERT_TRUE(client_conn.AsyncWrite(chunk1.data(), chunk1.size(), &w1_ctx));
    ASSERT_TRUE(client_conn.AsyncWrite(chunk2.data(), chunk2.size(), &w2_ctx));
    engine.SubmitAndWait(2);
    engine.ReapCompletions(events);

    std::array<char, 128> read_buf{};
    OpContext r_ctx;
    ASSERT_TRUE(server_conn.AsyncRead(read_buf.data(), read_buf.size(), &r_ctx));
    engine.SubmitAndWait(1);
    reaped = engine.ReapCompletions(events);
    ASSERT_EQ(reaped, 1u);
    EXPECT_EQ(std::string_view(read_buf.data(), events[0].BytesTransferred()), chunk1 + chunk2);
}

// 5. Client Disconnect (EOF on Read)
TEST_F(TcpNetIntegrationTest, PeerDisconnectReturnsEOF) {
    RingEngine engine(128);
    TcpListener listener(engine);
    ASSERT_TRUE(listener.BindAndListen("127.0.0.1", 0));

    sockaddr_in client_addr{};
    socklen_t client_addr_len = sizeof(client_addr);
    OpContext accept_ctx;
    ASSERT_TRUE(listener.AsyncAccept(&client_addr, &client_addr_len, &accept_ctx));

    sockaddr_in target_addr{};
    OpContext connect_ctx;
    TcpConnection client_conn(engine);
    ASSERT_TRUE(client_conn.AsyncConnect("127.0.0.1", listener.GetPort(), &target_addr, &connect_ctx));

    engine.SubmitAndWait(2);
    std::array<CompletionEvent, 8> events{};
    uint32_t reaped = engine.ReapCompletions(events);
    ASSERT_GE(reaped, 2u);

    int accepted_fd = -1;
    for (uint32_t i = 0; i < reaped; ++i) {
        if (events[i].context == &accept_ctx) accepted_fd = events[i].result;
    }
    ASSERT_GT(accepted_fd, 0);
    TcpConnection server_conn(engine, accepted_fd);

    // Prepare read on server
    std::array<char, 64> server_buf{};
    OpContext server_read_ctx;
    ASSERT_TRUE(server_conn.AsyncRead(server_buf.data(), server_buf.size(), &server_read_ctx));
    engine.Submit();

    // Client closes connection cleanly
    client_conn.Close();

    // Reaping read CQE on server should return 0 (EOF)
    engine.SubmitAndWait(1);
    reaped = engine.ReapCompletions(events);
    ASSERT_EQ(reaped, 1u);
    EXPECT_EQ(events[0].context, &server_read_ctx);
    EXPECT_TRUE(events[0].IsSuccess());
    EXPECT_EQ(events[0].result, 0); // 0 bytes read == EOF
}

// 6. Connection Refusal (Negative Path)
TEST_F(TcpNetIntegrationTest, ConnectionRefusedOnUnassignedPort) {
    RingEngine engine(64);
    TcpConnection client_conn(engine);
    sockaddr_in target_addr{};
    OpContext connect_ctx;

    // Try connecting to a loopback port where no listener exists (e.g. 59999)
    ASSERT_TRUE(client_conn.AsyncConnect("127.0.0.1", 59999, &target_addr, &connect_ctx));
    engine.SubmitAndWait(1);

    std::array<CompletionEvent, 4> events{};
    uint32_t reaped = engine.ReapCompletions(events);
    ASSERT_EQ(reaped, 1u);
    EXPECT_EQ(events[0].context, &connect_ctx);
    EXPECT_FALSE(events[0].IsSuccess());
    EXPECT_EQ(events[0].GetErrno(), ECONNREFUSED);
}

// 7. Read/Write Error on Closed Descriptor
TEST_F(TcpNetIntegrationTest, ReadWriteErrorOnClosedDescriptor) {
    RingEngine engine(64);
    TcpConnection conn(engine, -1); // Closed / invalid descriptor
    std::array<char, 32> buffer{};
    OpContext ctx;

    EXPECT_FALSE(conn.AsyncRead(buffer.data(), buffer.size(), &ctx));
    EXPECT_FALSE(conn.AsyncWrite(buffer.data(), buffer.size(), &ctx));
}

// 8. Orderly Half-Close (SHUT_WR)
TEST_F(TcpNetIntegrationTest, OrderlyHalfCloseShutdownWrite) {
    RingEngine engine(128);
    TcpListener listener(engine);
    ASSERT_TRUE(listener.BindAndListen("127.0.0.1", 0));

    sockaddr_in client_addr{};
    socklen_t client_addr_len = sizeof(client_addr);
    OpContext accept_ctx;
    ASSERT_TRUE(listener.AsyncAccept(&client_addr, &client_addr_len, &accept_ctx));

    sockaddr_in target_addr{};
    OpContext connect_ctx;
    TcpConnection client_conn(engine);
    ASSERT_TRUE(client_conn.AsyncConnect("127.0.0.1", listener.GetPort(), &target_addr, &connect_ctx));

    engine.SubmitAndWait(2);
    std::array<CompletionEvent, 8> events{};
    uint32_t reaped = engine.ReapCompletions(events);
    ASSERT_GE(reaped, 2u);

    int accepted_fd = -1;
    for (uint32_t i = 0; i < reaped; ++i) {
        if (events[i].context == &accept_ctx) accepted_fd = events[i].result;
    }
    ASSERT_GT(accepted_fd, 0);
    TcpConnection server_conn(engine, accepted_fd);

    // Prepare read on server
    std::array<char, 64> server_buf{};
    OpContext server_read_ctx;
    ASSERT_TRUE(server_conn.AsyncRead(server_buf.data(), server_buf.size(), &server_read_ctx));
    engine.Submit();

    // Client executes half-close (SHUT_WR)
    EXPECT_TRUE(client_conn.ShutdownWrite());

    // Server read CQE should receive EOF (result == 0)
    engine.SubmitAndWait(1);
    reaped = engine.ReapCompletions(events);
    ASSERT_EQ(reaped, 1u);
    EXPECT_EQ(events[0].result, 0);
}
