#include <gtest/gtest.h>
#include "io/ring_engine.hpp"
#include "io/op_context.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <array>
#include <cstring>

using namespace helios;

class RingEngineIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        int ret = pipe(pipe_fds_.data());
        ASSERT_EQ(ret, 0) << "Failed to create Linux pipe for integration testing";
    }

    void TearDown() override {
        if (pipe_fds_[0] != -1) close(pipe_fds_[0]);
        if (pipe_fds_[1] != -1) close(pipe_fds_[1]);
    }

    std::array<int, 2> pipe_fds_{-1, -1};
};

TEST_F(RingEngineIntegrationTest, RealKernelNopSubmissionAndReaping) {
    RingEngine engine(64);
    OpContext ctx;
    bool callback_executed = false;

    ctx.on_complete = [&](const CompletionEvent& evt) {
        callback_executed = true;
        EXPECT_EQ(evt.context, &ctx);
        EXPECT_TRUE(evt.IsSuccess());
        EXPECT_EQ(evt.result, 0);
    };

    ASSERT_TRUE(engine.PrepNop(&ctx));
    int submitted = engine.SubmitAndWait(1);
    EXPECT_EQ(submitted, 1);

    uint32_t processed = engine.ProcessCompletions();
    EXPECT_EQ(processed, 1u);
    EXPECT_TRUE(callback_executed);
    EXPECT_FALSE(ctx.in_flight);
}

TEST_F(RingEngineIntegrationTest, RealPipeWriteAndReadIO) {
    RingEngine engine(64);
    const std::string payload = "Helios io_uring integration payload 2026!";
    std::array<char, 128> read_buffer{};

    OpContext write_ctx;
    OpContext read_ctx;

    // 1. Submit Write to Pipe Write End
    ASSERT_TRUE(engine.PrepWrite(pipe_fds_[1], payload.data(), payload.size(), 0, &write_ctx));
    int submitted_write = engine.SubmitAndWait(1);
    EXPECT_EQ(submitted_write, 1);

    std::array<CompletionEvent, 4> events{};
    uint32_t reaped_write = engine.ReapCompletions(events);
    EXPECT_EQ(reaped_write, 1u);
    EXPECT_EQ(events[0].context, &write_ctx);
    EXPECT_TRUE(events[0].IsSuccess());
    EXPECT_EQ(events[0].BytesTransferred(), payload.size());
    EXPECT_FALSE(write_ctx.in_flight);

    // 2. Submit Read from Pipe Read End
    ASSERT_TRUE(engine.PrepRead(pipe_fds_[0], read_buffer.data(), read_buffer.size(), 0, &read_ctx));
    int submitted_read = engine.SubmitAndWait(1);
    EXPECT_EQ(submitted_read, 1);

    uint32_t reaped_read = engine.ReapCompletions(events);
    EXPECT_EQ(reaped_read, 1u);
    EXPECT_EQ(events[0].context, &read_ctx);
    EXPECT_TRUE(events[0].IsSuccess());
    EXPECT_EQ(events[0].BytesTransferred(), payload.size());
    EXPECT_EQ(std::string_view(read_buffer.data(), events[0].BytesTransferred()), payload);
    EXPECT_FALSE(read_ctx.in_flight);
}

TEST_F(RingEngineIntegrationTest, FailedOperationNegativePath) {
    RingEngine engine(64);
    OpContext ctx;
    std::array<char, 32> buffer{};
    bool callback_invoked = false;

    ctx.on_complete = [&](const CompletionEvent& evt) {
        callback_invoked = true;
        EXPECT_EQ(evt.context, &ctx);
        EXPECT_FALSE(evt.IsSuccess());
        EXPECT_EQ(evt.GetErrno(), EBADF);
    };

    // Submit read on invalid file descriptor (-1)
    ASSERT_TRUE(engine.PrepRead(-1, buffer.data(), buffer.size(), 0, &ctx));
    int submitted = engine.SubmitAndWait(1);
    EXPECT_EQ(submitted, 1);

    uint32_t processed = engine.ProcessCompletions();
    EXPECT_EQ(processed, 1u);
    EXPECT_TRUE(callback_invoked);
    EXPECT_FALSE(ctx.in_flight);
}
