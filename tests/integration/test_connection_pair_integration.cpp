#include <gtest/gtest.h>
#include "io/ring_engine.hpp"
#include "io/op_context.hpp"
#include "net/socket_utils.hpp"
#include "net/tcp_listener.hpp"
#include "net/tcp_connection.hpp"
#include "net/connection_pair.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <array>
#include <string>
#include <chrono>

using namespace helios;

class ConnectionPairIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// 1. Normal Close Integration Test
TEST_F(ConnectionPairIntegrationTest, NormalCloseLifecycle) {
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

    auto conn_pair = std::make_shared<ConnectionPair>(101, engine, client_conn.GetFd());
    conn_pair->SetBackendFd(accepted_fd);
    conn_pair->TransitionTo(ConnectionState::Established);
    EXPECT_EQ(conn_pair->GetState(), ConnectionState::Established);

    // Normal close
    conn_pair->InitiateClose("Normal close");
    EXPECT_TRUE(conn_pair->IsClosed());
}

// 2. Client Half-Close Integration Test
TEST_F(ConnectionPairIntegrationTest, ClientHalfCloseFlow) {
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

    auto conn_pair = std::make_shared<ConnectionPair>(102, engine, client_conn.GetFd());
    conn_pair->SetBackendFd(accepted_fd);
    conn_pair->TransitionTo(ConnectionState::Established);

    // Client sends FIN via shutdown(SHUT_WR)
    client_conn.ShutdownWrite();

    // Server reads EOF (0 bytes) from client
    std::array<char, 64> read_buf{};
    OpContext server_read_ctx;
    engine.PrepRead(accepted_fd, read_buf.data(), read_buf.size(), 0, &server_read_ctx);
    engine.SubmitAndWait(1);
    reaped = engine.ReapCompletions(events);
    ASSERT_EQ(reaped, 1u);
    EXPECT_EQ(events[0].result, 0); // 0 bytes == EOF

    // Handle Client FIN in state machine
    conn_pair->HandleClientFin();
    EXPECT_EQ(conn_pair->GetState(), ConnectionState::HalfClosedClient);
    EXPECT_TRUE(conn_pair->IsClientReadStopped());

    // Backend responds and then closes
    conn_pair->HandleBackendFin();
    EXPECT_TRUE(conn_pair->IsClosed());
}

// 3. Backend Half-Close Integration Test
TEST_F(ConnectionPairIntegrationTest, BackendHalfCloseFlow) {
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

    auto conn_pair = std::make_shared<ConnectionPair>(103, engine, client_conn.GetFd());
    conn_pair->SetBackendFd(accepted_fd);
    conn_pair->TransitionTo(ConnectionState::Established);

    // Backend half-closes (SHUT_WR)
    ::shutdown(accepted_fd, SHUT_WR);

    // Client reads EOF
    std::array<char, 64> read_buf{};
    OpContext client_read_ctx;
    engine.PrepRead(client_conn.GetFd(), read_buf.data(), read_buf.size(), 0, &client_read_ctx);
    engine.SubmitAndWait(1);
    reaped = engine.ReapCompletions(events);
    ASSERT_EQ(reaped, 1u);
    EXPECT_EQ(events[0].result, 0);

    // Handle Backend FIN in state machine
    conn_pair->HandleBackendFin();
    EXPECT_EQ(conn_pair->GetState(), ConnectionState::HalfClosedBackend);
    EXPECT_TRUE(conn_pair->IsBackendReadStopped());

    // Client finishes and closes
    conn_pair->HandleClientFin();
    EXPECT_TRUE(conn_pair->IsClosed());
}

// 4. Simultaneous Half-Close Integration Test
TEST_F(ConnectionPairIntegrationTest, SimultaneousHalfCloseFlow) {
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

    auto conn_pair = std::make_shared<ConnectionPair>(104, engine, client_conn.GetFd());
    conn_pair->SetBackendFd(accepted_fd);
    conn_pair->TransitionTo(ConnectionState::Established);

    // Both client and backend initiate half-close concurrently
    conn_pair->HandleClientFin();
    EXPECT_EQ(conn_pair->GetState(), ConnectionState::HalfClosedClient);

    conn_pair->HandleBackendFin();
    EXPECT_TRUE(conn_pair->IsClosed());
}

// 5. Close with Outstanding I/O Integration Test
TEST_F(ConnectionPairIntegrationTest, CloseWithOutstandingIOPreservesMemory) {
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

    bool cleanup_executed = false;
    auto conn_pair = std::make_shared<ConnectionPair>(105, engine, client_conn.GetFd());
    conn_pair->SetBackendFd(accepted_fd);
    conn_pair->SetCleanupCallback([&](uint64_t) { cleanup_executed = true; });
    conn_pair->TransitionTo(ConnectionState::Established);

    // Prepare read operation on client_fd inside io_uring
    std::array<char, 64> read_buf{};
    OpContext read_ctx;
    conn_pair->IncrementPendingCqe(); // Track in-flight CQE
    engine.PrepRead(client_conn.GetFd(), read_buf.data(), read_buf.size(), 0, &read_ctx);
    engine.Submit();

    // Initiate close while io_uring operation is active in kernel
    conn_pair->InitiateClose("Close during active read");

    // State MUST be PendingCancellation and cleanup MUST NOT have run yet
    EXPECT_EQ(conn_pair->GetState(), ConnectionState::PendingCancellation);
    EXPECT_FALSE(cleanup_executed);

    // Now reap the canceled/closed read CQE from io_uring
    engine.SubmitAndWait(1);
    reaped = engine.ReapCompletions(events);
    ASSERT_EQ(reaped, 1u);

    // Decrement pending CQE counter upon reaping
    conn_pair->DecrementPendingCqe();

    // Now state MUST transition to Closed and cleanup MUST run safely
    EXPECT_EQ(conn_pair->GetState(), ConnectionState::Closed);
    EXPECT_TRUE(cleanup_executed);
}

// 6. Timeout During Draining Integration Test
TEST_F(ConnectionPairIntegrationTest, HalfCloseTimeoutDuringDraining) {
    RingEngine engine(128);
    auto conn_pair = std::make_shared<ConnectionPair>(106, engine, -1);
    conn_pair->TransitionTo(ConnectionState::Established);

    conn_pair->HandleClientFin();
    EXPECT_EQ(conn_pair->GetState(), ConnectionState::HalfClosedClient);

    auto start_time = ConnectionPair::Clock::now();
    // Simulate 6s passing for a 5s half-close timeout
    bool timed_out = conn_pair->CheckHalfCloseTimeout(start_time + std::chrono::seconds(6), std::chrono::seconds(5));
    EXPECT_TRUE(timed_out);
    EXPECT_TRUE(conn_pair->IsClosed());
}

// 7. Repeated Close Events Integration Test
TEST_F(ConnectionPairIntegrationTest, RepeatedCloseEventsIdempotency) {
    RingEngine engine(128);
    auto conn_pair = std::make_shared<ConnectionPair>(107, engine, -1);
    conn_pair->TransitionTo(ConnectionState::Established);

    conn_pair->InitiateClose("Close 1");
    EXPECT_TRUE(conn_pair->IsClosed());

    // Duplicate close requests must be safe and idempotent
    conn_pair->InitiateClose("Close 2");
    conn_pair->InitiateClose("Close 3");
    EXPECT_TRUE(conn_pair->IsClosed());
}
