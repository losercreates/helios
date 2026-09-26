#include <gtest/gtest.h>
#include "io/ring_engine.hpp"
#include "io/op_context.hpp"
#include <array>
#include <system_error>

using namespace helios;

TEST(RingEngineUnitTest, InitializationAndTeardown) {
    RingEngine engine(64);
    EXPECT_TRUE(engine.IsInitialized());
    EXPECT_EQ(engine.Capacity(), 64u);
    EXPECT_EQ(engine.PendingSQEs(), 0u);
    EXPECT_EQ(engine.InFlightOps(), 0u);

    engine.Shutdown();
    EXPECT_FALSE(engine.IsInitialized());
}

TEST(RingEngineUnitTest, MoveSemantics) {
    RingEngine engine1(128);
    EXPECT_TRUE(engine1.IsInitialized());
    EXPECT_EQ(engine1.Capacity(), 128u);

    RingEngine engine2(std::move(engine1));
    EXPECT_FALSE(engine1.IsInitialized());
    EXPECT_TRUE(engine2.IsInitialized());
    EXPECT_EQ(engine2.Capacity(), 128u);
}

TEST(RingEngineUnitTest, PrepNopAndSQETagging) {
    RingEngine engine(64);
    OpContext ctx;
    ctx.opaque_user_data = reinterpret_cast<void*>(0xDEADBEEF);

    EXPECT_FALSE(ctx.in_flight);
    EXPECT_TRUE(engine.PrepNop(&ctx));

    EXPECT_TRUE(ctx.in_flight);
    EXPECT_EQ(ctx.type, OpType::Nop);
    EXPECT_EQ(engine.PendingSQEs(), 1u);
    EXPECT_EQ(engine.InFlightOps(), 1u);
}

TEST(RingEngineUnitTest, PrepReadAndWriteMetadata) {
    RingEngine engine(64);
    OpContext read_ctx;
    OpContext write_ctx;
    std::array<char, 128> buffer{};

    EXPECT_TRUE(engine.PrepRead(10, buffer.data(), buffer.size(), 0, &read_ctx));
    EXPECT_EQ(read_ctx.type, OpType::Read);
    EXPECT_EQ(read_ctx.fd, 10);
    EXPECT_EQ(read_ctx.buffer, buffer.data());
    EXPECT_EQ(read_ctx.bytes_requested, buffer.size());

    EXPECT_TRUE(engine.PrepWrite(11, buffer.data(), buffer.size(), 0, &write_ctx));
    EXPECT_EQ(write_ctx.type, OpType::Write);
    EXPECT_EQ(write_ctx.fd, 11);
    EXPECT_EQ(write_ctx.buffer, buffer.data());
    EXPECT_EQ(write_ctx.bytes_requested, buffer.size());

    EXPECT_EQ(engine.PendingSQEs(), 2u);
    EXPECT_EQ(engine.InFlightOps(), 2u);
}

TEST(RingEngineUnitTest, OpContextCompletionEventHelpers) {
    OpContext ctx;
    ctx.type = OpType::Read;
    ctx.bytes_requested = 100;

    CompletionEvent success_evt{.context = &ctx, .result = 42, .flags = 0};
    EXPECT_TRUE(success_evt.IsSuccess());
    EXPECT_EQ(success_evt.GetErrno(), 0);
    EXPECT_EQ(success_evt.BytesTransferred(), 42u);

    CompletionEvent error_evt{.context = &ctx, .result = -EBADF, .flags = 0};
    EXPECT_FALSE(error_evt.IsSuccess());
    EXPECT_EQ(error_evt.GetErrno(), EBADF);
    EXPECT_EQ(error_evt.BytesTransferred(), 0u);
}

TEST(RingEngineUnitTest, UninitializedRingErrorHandling) {
    RingEngine engine(64);
    engine.Shutdown();

    OpContext ctx;
    EXPECT_FALSE(engine.PrepNop(&ctx));
    EXPECT_EQ(engine.Submit(), -1);
    EXPECT_EQ(engine.SubmitAndWait(1), -1);

    std::array<CompletionEvent, 4> events{};
    EXPECT_EQ(engine.ReapCompletions(events), 0u);
}
