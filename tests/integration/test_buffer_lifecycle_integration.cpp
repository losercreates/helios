#include <gtest/gtest.h>
#include "io/ring_engine.hpp"
#include "io/op_context.hpp"
#include "buffer/buffer_pool.hpp"
#include "net/socket_utils.hpp"
#include "net/tcp_listener.hpp"
#include "net/tcp_connection.hpp"
#include "net/connection_pair.hpp"
#include <unistd.h>
#include <sys/socket.h>
#include <array>
#include <vector>
#include <memory>
#include <chrono>
#include <cstring>

using namespace helios;

class BufferLifecycleIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// 1. Connection Close with Outstanding I/O & Late CQE Reclamation
TEST_F(BufferLifecycleIntegrationTest, LateCqeReclaimsBufferAfterConnectionClose) {
    RingEngine engine(128);
    BufferPool pool(10, 1024);
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

    // Acquire buffer from pool for read operation
    Buffer* buf = pool.Acquire();
    ASSERT_NE(buf, nullptr);
    EXPECT_EQ(buf->ref_count, 1u);
    EXPECT_EQ(pool.CheckedOutCount(), 1u);

    // Prepare read operation using acquired buffer
    OpContext read_ctx;
    read_ctx.opaque_user_data = buf;
    engine.PrepRead(client_conn.GetFd(), buf->data, buf->capacity, 0, &read_ctx);
    engine.Submit();

    // Close client connection while read SQE is active in kernel
    client_conn.Close();
    close(accepted_fd);

    // Buffer MUST NOT be freed or returned to pool yet!
    EXPECT_EQ(pool.CheckedOutCount(), 1u);
    EXPECT_TRUE(buf->is_checked_out);

    // Reap late CQE from kernel
    engine.SubmitAndWait(1);
    reaped = engine.ReapCompletions(events);
    ASSERT_EQ(reaped, 1u);

    // Process late CQE completion and release buffer
    auto* completed_buf = static_cast<Buffer*>(events[0].context->opaque_user_data);
    EXPECT_EQ(completed_buf, buf);
    bool returned = pool.Release(completed_buf);
    EXPECT_TRUE(returned);

    // Now buffer MUST be cleanly returned to pool!
    EXPECT_EQ(pool.CheckedOutCount(), 0u);
    EXPECT_FALSE(buf->is_checked_out);
}

// 2. Multiple Outstanding Operations referencing same/multiple buffers
TEST_F(BufferLifecycleIntegrationTest, MultipleOutstandingOperationsBufferLifetime) {
    RingEngine engine(128);
    BufferPool pool(10, 1024);
    std::array<int, 2> pipe_fds{-1, -1};
    ASSERT_EQ(pipe(pipe_fds.data()), 0);

    Buffer* buf1 = pool.Acquire();
    Buffer* buf2 = pool.Acquire();
    ASSERT_NE(buf1, nullptr);
    ASSERT_NE(buf2, nullptr);
    EXPECT_EQ(pool.CheckedOutCount(), 2u);

    OpContext w_ctx, r_ctx;
    w_ctx.opaque_user_data = buf1;
    r_ctx.opaque_user_data = buf2;

    std::string data = "BufferLifetimeIntegrationTest";
    std::memcpy(buf1->data, data.data(), data.size());
    buf1->AdvanceWrite(data.size());

    engine.PrepWrite(pipe_fds[1], buf1->data, buf1->write_offset, 0, &w_ctx);
    engine.PrepRead(pipe_fds[0], buf2->data, buf2->capacity, 0, &r_ctx);
    engine.SubmitAndWait(2);

    std::array<CompletionEvent, 8> events{};
    uint32_t reaped = engine.ReapCompletions(events);
    EXPECT_EQ(reaped, 2u);

    for (uint32_t i = 0; i < reaped; ++i) {
        auto* b = static_cast<Buffer*>(events[i].context->opaque_user_data);
        pool.Release(b);
    }

    EXPECT_EQ(pool.CheckedOutCount(), 0u);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

// 3. Stress Test: Repeated Connection Creation & Closure with Outstanding Async I/O
TEST_F(BufferLifecycleIntegrationTest, StressRepeatedConnectionCloseWithInFlightIO) {
    RingEngine engine(256);
    BufferPool pool(32, 1024);

    for (int iteration = 0; iteration < 50; ++iteration) {
        std::array<int, 2> sv{-1, -1};
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv.data()), 0);

        Buffer* buf = pool.Acquire();
        ASSERT_NE(buf, nullptr);

        OpContext read_ctx;
        read_ctx.opaque_user_data = buf;
        engine.PrepRead(sv[0], buf->data, buf->capacity, 0, &read_ctx);
        engine.Submit();

        // Close sockets while read is pending in kernel
        close(sv[1]);
        close(sv[0]);

        // Reap CQE
        engine.SubmitAndWait(1);
        std::array<CompletionEvent, 4> events{};
        uint32_t reaped = engine.ReapCompletions(events);
        ASSERT_EQ(reaped, 1u);

        auto* b = static_cast<Buffer*>(events[0].context->opaque_user_data);
        EXPECT_TRUE(pool.Release(b));
    }

    // All 50 iterations complete with 0 buffer leaks!
    EXPECT_EQ(pool.CheckedOutCount(), 0u);
    EXPECT_EQ(pool.FreeCount(), 32u);
}
