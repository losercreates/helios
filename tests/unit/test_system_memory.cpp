#include <gtest/gtest.h>
#include "metrics/system_memory.hpp"
#include "metrics/metrics_collector.hpp"
#include "buffer/buffer_pool.hpp"

using namespace helios;

TEST(SystemMemoryTest, QueryProcessMemoryReturnsPositiveValues) {
    ProcessMemoryInfo info = QueryProcessMemory();
    EXPECT_GT(info.resident_bytes, 0u);
    EXPECT_GT(info.virtual_bytes, 0u);
    EXPECT_LE(info.resident_bytes, info.virtual_bytes + 1024 * 1024 * 1024); // Sanity check
}

TEST(SystemMemoryTest, BufferPoolMemoryQueries) {
    BufferPool pool(128, 4096);
    EXPECT_EQ(pool.TotalBuffers(), 128u);
    EXPECT_EQ(pool.BlockSize(), 4096u);
    EXPECT_EQ(pool.TotalAllocatedBytes(), 128u * 4096u);
    EXPECT_EQ(pool.ActiveAllocatedBytes(), 0u);

    Buffer* b1 = pool.Acquire();
    ASSERT_NE(b1, nullptr);
    EXPECT_EQ(pool.ActiveAllocatedBytes(), 4096u);

    Buffer* b2 = pool.Acquire();
    ASSERT_NE(b2, nullptr);
    EXPECT_EQ(pool.ActiveAllocatedBytes(), 8192u);

    pool.Release(b1);
    EXPECT_EQ(pool.ActiveAllocatedBytes(), 4096u);

    pool.Release(b2);
    EXPECT_EQ(pool.ActiveAllocatedBytes(), 0u);
}

TEST(SystemMemoryTest, PrometheusMetricsExportIncludesMemoryGauges) {
    MetricsCollector collector;
    auto w0 = collector.GetOrCreateWorkerMetrics(0);
    w0->buffer_pool_bytes_allocated.store(16384 * 64, std::memory_order_relaxed);
    w0->buffer_pool_bytes_used.store(16384 * 8, std::memory_order_relaxed);
    w0->backend_pool_idle_connections.store(5, std::memory_order_relaxed);

    std::string text = collector.RenderPrometheusText();

    EXPECT_NE(text.find("helios_process_resident_memory_bytes"), std::string::npos);
    EXPECT_NE(text.find("helios_process_virtual_memory_bytes"), std::string::npos);
    EXPECT_NE(text.find("helios_buffer_pool_bytes_allocated{worker=\"0\"} 1048576"), std::string::npos);
    EXPECT_NE(text.find("helios_buffer_pool_bytes_used{worker=\"0\"} 131072"), std::string::npos);
    EXPECT_NE(text.find("helios_connection_pool_idle_connections{worker=\"0\"} 5"), std::string::npos);
}
