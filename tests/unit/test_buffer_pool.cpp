#include <gtest/gtest.h>
#include "buffer/buffer_pool.hpp"
#include "buffer/backpressure_controller.hpp"
#include <vector>

using namespace helios;

TEST(BufferPoolUnitTest, AcquireAndReleaseBasic) {
    BufferPool pool(10, 1024);
    EXPECT_EQ(pool.TotalBuffers(), 10u);
    EXPECT_EQ(pool.FreeCount(), 10u);
    EXPECT_EQ(pool.CheckedOutCount(), 0u);

    Buffer* buf = pool.Acquire();
    ASSERT_NE(buf, nullptr);
    EXPECT_EQ(buf->capacity, 1024u);
    EXPECT_EQ(buf->ref_count, 1u);
    EXPECT_TRUE(buf->is_checked_out);
    EXPECT_EQ(pool.CheckedOutCount(), 1u);
    EXPECT_EQ(pool.FreeCount(), 9u);

    EXPECT_TRUE(pool.Release(buf));
    EXPECT_FALSE(buf->is_checked_out);
    EXPECT_EQ(pool.CheckedOutCount(), 0u);
    EXPECT_EQ(pool.FreeCount(), 10u);
}

TEST(BufferPoolUnitTest, PoolExhaustionAndRecovery) {
    BufferPool pool(5, 512);
    std::vector<Buffer*> acquired;

    for (size_t i = 0; i < 5; ++i) {
        Buffer* buf = pool.Acquire();
        ASSERT_NE(buf, nullptr);
        acquired.push_back(buf);
    }

    EXPECT_TRUE(pool.IsExhausted());
    EXPECT_EQ(pool.FreeCount(), 0u);
    EXPECT_EQ(pool.Acquire(), nullptr); // Pool is exhausted

    // Release 2 buffers
    EXPECT_TRUE(pool.Release(acquired[0]));
    EXPECT_TRUE(pool.Release(acquired[1]));
    EXPECT_FALSE(pool.IsExhausted());
    EXPECT_EQ(pool.FreeCount(), 2u);

    // Can acquire again
    Buffer* new_buf1 = pool.Acquire();
    Buffer* new_buf2 = pool.Acquire();
    ASSERT_NE(new_buf1, nullptr);
    ASSERT_NE(new_buf2, nullptr);
    EXPECT_TRUE(pool.IsExhausted());
}

TEST(BufferPoolUnitTest, HighLowWatermarkBackpressure) {
    // 10 buffers, high watermark = 1.0 (10/10), low watermark = 0.8 (8/10)
    BufferPool pool(10, 512, 1.0, 0.8);
    BackpressureController controller(pool);

    std::vector<Buffer*> acquired;
    for (size_t i = 0; i < 9; ++i) {
        acquired.push_back(pool.Acquire());
        controller.Evaluate();
        EXPECT_FALSE(controller.IsEngaged());
    }

    // 10th acquire hits 100% (high watermark)
    acquired.push_back(pool.Acquire());
    controller.Evaluate();
    EXPECT_TRUE(pool.IsHighWatermarkReached());
    EXPECT_TRUE(controller.IsEngaged());
    EXPECT_TRUE(controller.AreReadsPaused());
    EXPECT_TRUE(controller.AreAcceptsPaused());

    // Release 1 buffer -> 9/10 (90%) - still above low watermark (80%)
    pool.Release(acquired.back());
    acquired.pop_back();
    controller.Evaluate();
    EXPECT_TRUE(controller.IsEngaged());

    // Release 1 more buffer -> 8/10 (80%) - hits low watermark threshold
    pool.Release(acquired.back());
    acquired.pop_back();
    controller.Evaluate();
    EXPECT_TRUE(pool.IsLowWatermarkResumed());
    EXPECT_FALSE(controller.IsEngaged()); // Backpressure disengaged!
    EXPECT_FALSE(controller.AreReadsPaused());
    EXPECT_FALSE(controller.AreAcceptsPaused());
}

TEST(BufferPoolUnitTest, DeferredReclamationWithRefCount) {
    BufferPool pool(5, 512);
    Buffer* buf = pool.Acquire();
    ASSERT_NE(buf, nullptr);
    EXPECT_EQ(buf->ref_count, 1u);

    // Retain buffer (simulate second in-flight io_uring operation)
    pool.Retain(buf);
    EXPECT_EQ(buf->ref_count, 2u);

    // First release (e.g. first CQE reaped)
    EXPECT_FALSE(pool.Release(buf)); // Returns false: still in use
    EXPECT_TRUE(buf->is_checked_out);
    EXPECT_EQ(pool.CheckedOutCount(), 1u);

    // Second release (e.g. second CQE reaped)
    EXPECT_TRUE(pool.Release(buf)); // Returns true: actually returned to pool
    EXPECT_FALSE(buf->is_checked_out);
    EXPECT_EQ(pool.CheckedOutCount(), 0u);
}

TEST(BufferPoolUnitTest, DoubleReleasePrevention) {
    BufferPool pool(5, 512);
    Buffer* buf = pool.Acquire();
    ASSERT_NE(buf, nullptr);

    EXPECT_TRUE(pool.Release(buf));
    EXPECT_FALSE(buf->is_checked_out);

    // Attempt double-release
    EXPECT_FALSE(pool.Release(buf));
    EXPECT_EQ(pool.FreeCount(), 5u); // Free stack remains uncorrupted
}

TEST(BufferPoolUnitTest, BufferReadWriteCursors) {
    BufferPool pool(1, 1024);
    Buffer* buf = pool.Acquire();
    ASSERT_NE(buf, nullptr);

    EXPECT_EQ(buf->ReadableBytes(), 0u);
    EXPECT_EQ(buf->WritableBytes(), 1024u);

    buf->AdvanceWrite(100);
    EXPECT_EQ(buf->ReadableBytes(), 100u);
    EXPECT_EQ(buf->WritableBytes(), 924u);

    buf->AdvanceRead(40);
    EXPECT_EQ(buf->ReadableBytes(), 60u);
    EXPECT_EQ(buf->WritableBytes(), 924u);
}
