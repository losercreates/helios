#include <gtest/gtest.h>
#include "backend/backend_pool.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#include <algorithm>

using namespace helios;

namespace {

// Helper to check if a real file descriptor is valid/open
bool IsFdOpen(int fd) {
    return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

} // namespace

// ============================================================================
// 1. Connection Acquisition and Release
// ============================================================================

TEST(BackendPoolAcquisitionAndRelease, AcquireWhenEmpty) {
    BackendConnectionPool pool;
    EXPECT_EQ(pool.TotalIdleCount(), 0u);
    EXPECT_EQ(pool.IdleCountForBackend(1), 0u);

    auto result = pool.Acquire(1);
    EXPECT_FALSE(result.has_value());
}

TEST(BackendPoolAcquisitionAndRelease, ReleaseAndReacquire) {
    std::vector<int> closed_fds;
    BackendConnectionPool pool({}, [&](int fd) {
        closed_fds.push_back(fd);
    });

    int test_fd = 42;
    auto now = BackendConnectionPool::Clock::now();

    EXPECT_TRUE(pool.Release(1, test_fd, now));
    EXPECT_EQ(pool.TotalIdleCount(), 1u);
    EXPECT_EQ(pool.IdleCountForBackend(1), 1u);

    auto acquired = pool.Acquire(1, now);
    ASSERT_TRUE(acquired.has_value());
    EXPECT_EQ(*acquired, test_fd);

    EXPECT_EQ(pool.TotalIdleCount(), 0u);
    EXPECT_EQ(pool.IdleCountForBackend(1), 0u);
    EXPECT_TRUE(closed_fds.empty());
}

TEST(BackendPoolAcquisitionAndRelease, MultipleBackendsIndependence) {
    std::vector<int> closed_fds;
    BackendConnectionPool pool({}, [&](int fd) {
        closed_fds.push_back(fd);
    });

    pool.Release(101, 10);
    pool.Release(102, 20);

    EXPECT_EQ(pool.TotalIdleCount(), 2u);
    EXPECT_EQ(pool.IdleCountForBackend(101), 1u);
    EXPECT_EQ(pool.IdleCountForBackend(102), 1u);

    auto acq1 = pool.Acquire(101);
    ASSERT_TRUE(acq1.has_value());
    EXPECT_EQ(*acq1, 10);

    EXPECT_EQ(pool.TotalIdleCount(), 1u);
    EXPECT_EQ(pool.IdleCountForBackend(101), 0u);
    EXPECT_EQ(pool.IdleCountForBackend(102), 1u);

    auto acq2 = pool.Acquire(102);
    ASSERT_TRUE(acq2.has_value());
    EXPECT_EQ(*acq2, 20);

    EXPECT_EQ(pool.TotalIdleCount(), 0u);
}

TEST(BackendPoolAcquisitionAndRelease, LIFOOrderingReusesMostRecent) {
    std::vector<int> closed_fds;
    BackendConnectionPool pool({}, [&](int fd) {
        closed_fds.push_back(fd);
    });

    auto t0 = BackendConnectionPool::Clock::now();
    pool.Release(1, 10, t0);
    pool.Release(1, 20, t0 + std::chrono::milliseconds(10));
    pool.Release(1, 30, t0 + std::chrono::milliseconds(20));

    EXPECT_EQ(pool.TotalIdleCount(), 3u);

    // LIFO: most recently released should be popped first
    auto acq1 = pool.Acquire(1, t0 + std::chrono::milliseconds(30));
    ASSERT_TRUE(acq1.has_value());
    EXPECT_EQ(*acq1, 30);

    auto acq2 = pool.Acquire(1, t0 + std::chrono::milliseconds(30));
    ASSERT_TRUE(acq2.has_value());
    EXPECT_EQ(*acq2, 20);

    auto acq3 = pool.Acquire(1, t0 + std::chrono::milliseconds(30));
    ASSERT_TRUE(acq3.has_value());
    EXPECT_EQ(*acq3, 10);

    EXPECT_EQ(pool.TotalIdleCount(), 0u);
}

TEST(BackendPoolAcquisitionAndRelease, InvalidNegativeFdRejected) {
    BackendConnectionPool pool;
    EXPECT_FALSE(pool.Release(1, -1));
    EXPECT_EQ(pool.TotalIdleCount(), 0u);
}

// ============================================================================
// 2. Capacity Limits and Eviction
// ============================================================================

TEST(BackendPoolCapacityLimitsAndEviction, PerBackendCapacityEvictsOldest) {
    std::vector<int> closed_fds;
    BackendPoolConfig config;
    config.max_idle_per_backend = 2;
    config.max_total_idle = 100;

    BackendConnectionPool pool(config, [&](int fd) {
        closed_fds.push_back(fd);
    });

    auto t0 = BackendConnectionPool::Clock::now();
    pool.Release(1, 100, t0);
    pool.Release(1, 101, t0 + std::chrono::milliseconds(10));

    EXPECT_EQ(pool.TotalIdleCount(), 2u);
    EXPECT_EQ(pool.IdleCountForBackend(1), 2u);
    EXPECT_TRUE(closed_fds.empty());

    // Releasing a 3rd socket must evict the oldest (FD 100)
    pool.Release(1, 102, t0 + std::chrono::milliseconds(20));

    EXPECT_EQ(pool.TotalIdleCount(), 2u);
    EXPECT_EQ(pool.IdleCountForBackend(1), 2u);
    ASSERT_EQ(closed_fds.size(), 1u);
    EXPECT_EQ(closed_fds[0], 100);

    // Remaining should be 102 (newest) and 101 (older)
    auto acq1 = pool.Acquire(1, t0 + std::chrono::milliseconds(30));
    ASSERT_TRUE(acq1.has_value());
    EXPECT_EQ(*acq1, 102);

    auto acq2 = pool.Acquire(1, t0 + std::chrono::milliseconds(30));
    ASSERT_TRUE(acq2.has_value());
    EXPECT_EQ(*acq2, 101);
}

TEST(BackendPoolCapacityLimitsAndEviction, GlobalCapacityEvictsOldestGlobally) {
    std::vector<int> closed_fds;
    BackendPoolConfig config;
    config.max_idle_per_backend = 10;
    config.max_total_idle = 3;

    BackendConnectionPool pool(config, [&](int fd) {
        closed_fds.push_back(fd);
    });

    auto t0 = BackendConnectionPool::Clock::now();
    pool.Release(1, 10, t0);
    pool.Release(2, 20, t0 + std::chrono::milliseconds(5));
    pool.Release(3, 30, t0 + std::chrono::milliseconds(10));

    EXPECT_EQ(pool.TotalIdleCount(), 3u);
    EXPECT_TRUE(closed_fds.empty());

    // 4th connection exceeds max_total_idle (3).
    // FD 10 on backend 1 was released earliest (at t0), so it must be evicted.
    pool.Release(4, 40, t0 + std::chrono::milliseconds(15));

    EXPECT_EQ(pool.TotalIdleCount(), 3u);
    ASSERT_EQ(closed_fds.size(), 1u);
    EXPECT_EQ(closed_fds[0], 10);
    EXPECT_EQ(pool.IdleCountForBackend(1), 0u);
    EXPECT_EQ(pool.IdleCountForBackend(4), 1u);
}

TEST(BackendPoolCapacityLimitsAndEviction, ZeroCapacityImmediatelyCloses) {
    std::vector<int> closed_fds;
    BackendPoolConfig config;
    config.max_idle_per_backend = 0;
    config.max_total_idle = 10;

    BackendConnectionPool pool(config, [&](int fd) {
        closed_fds.push_back(fd);
    });

    EXPECT_FALSE(pool.Release(1, 999));
    EXPECT_EQ(pool.TotalIdleCount(), 0u);
    ASSERT_EQ(closed_fds.size(), 1u);
    EXPECT_EQ(closed_fds[0], 999);
}

// ============================================================================
// 3. Idle Timeout Reaping
// ============================================================================

TEST(BackendPoolIdleTimeoutReaping, AcquireDetectsAndClosesExpiredConnection) {
    std::vector<int> closed_fds;
    BackendPoolConfig config;
    config.idle_timeout = std::chrono::milliseconds(500);

    BackendConnectionPool pool(config, [&](int fd) {
        closed_fds.push_back(fd);
    });

    auto t0 = BackendConnectionPool::Clock::now();
    pool.Release(1, 10, t0);
    EXPECT_EQ(pool.TotalIdleCount(), 1u);

    // Acquire after 600ms (exceeds 500ms timeout)
    auto acq = pool.Acquire(1, t0 + std::chrono::milliseconds(600));
    EXPECT_FALSE(acq.has_value());
    EXPECT_EQ(pool.TotalIdleCount(), 0u);
    ASSERT_EQ(closed_fds.size(), 1u);
    EXPECT_EQ(closed_fds[0], 10);
}

TEST(BackendPoolIdleTimeoutReaping, ReapExpiredConnectionsMethod) {
    std::vector<int> closed_fds;
    BackendPoolConfig config;
    config.idle_timeout = std::chrono::milliseconds(1000);

    BackendConnectionPool pool(config, [&](int fd) {
        closed_fds.push_back(fd);
    });

    auto t0 = BackendConnectionPool::Clock::now();
    // Backend 1 has 2 connections: one old (t0), one recent (t0 + 600ms)
    pool.Release(1, 11, t0);
    pool.Release(1, 12, t0 + std::chrono::milliseconds(600));

    // Backend 2 has 1 old connection (t0 + 100ms)
    pool.Release(2, 21, t0 + std::chrono::milliseconds(100));

    EXPECT_EQ(pool.TotalIdleCount(), 3u);

    // At t0 + 1050ms:
    // FD 11 age is 1050ms >= 1000ms -> expired
    // FD 21 age is 950ms < 1000ms -> NOT expired yet
    // FD 12 age is 450ms < 1000ms -> NOT expired yet
    size_t reaped = pool.ReapExpired(t0 + std::chrono::milliseconds(1050));
    EXPECT_EQ(reaped, 1u);
    EXPECT_EQ(pool.TotalIdleCount(), 2u);
    ASSERT_EQ(closed_fds.size(), 1u);
    EXPECT_EQ(closed_fds[0], 11);

    // At t0 + 1200ms:
    // FD 21 age is 1100ms >= 1000ms -> expired
    // FD 12 age is 600ms < 1000ms -> NOT expired
    reaped = pool.ReapExpired(t0 + std::chrono::milliseconds(1200));
    EXPECT_EQ(reaped, 1u);
    EXPECT_EQ(pool.TotalIdleCount(), 1u);
    ASSERT_EQ(closed_fds.size(), 2u);
    EXPECT_EQ(closed_fds[1], 21);

    // Only FD 12 remains
    auto acq = pool.Acquire(1, t0 + std::chrono::milliseconds(1200));
    ASSERT_TRUE(acq.has_value());
    EXPECT_EQ(*acq, 12);
}

// ============================================================================
// 4. Health State Purge & Destruction Safety
// ============================================================================

TEST(BackendPoolHealthStatePurge, PurgeSpecificBackendOnDrainingOrUnhealthy) {
    std::vector<int> closed_fds;
    BackendConnectionPool pool({}, [&](int fd) {
        closed_fds.push_back(fd);
    });

    pool.Release(1, 10);
    pool.Release(1, 11);
    pool.Release(2, 20);

    EXPECT_EQ(pool.TotalIdleCount(), 3u);
    EXPECT_EQ(pool.IdleCountForBackend(1), 2u);
    EXPECT_EQ(pool.IdleCountForBackend(2), 1u);

    // Backend 1 goes unhealthy or begins draining
    size_t purged = pool.Purge(1);
    EXPECT_EQ(purged, 2u);
    EXPECT_EQ(pool.TotalIdleCount(), 1u);
    EXPECT_EQ(pool.IdleCountForBackend(1), 0u);
    EXPECT_EQ(pool.IdleCountForBackend(2), 1u);

    // FDs 10 and 11 closed
    EXPECT_EQ(closed_fds.size(), 2u);
    EXPECT_TRUE(std::find(closed_fds.begin(), closed_fds.end(), 10) != closed_fds.end());
    EXPECT_TRUE(std::find(closed_fds.begin(), closed_fds.end(), 11) != closed_fds.end());

    // Backend 2 remains intact
    auto acq = pool.Acquire(2);
    ASSERT_TRUE(acq.has_value());
    EXPECT_EQ(*acq, 20);
}

TEST(BackendPoolHealthStatePurge, ClearClosesAll) {
    std::vector<int> closed_fds;
    BackendConnectionPool pool({}, [&](int fd) {
        closed_fds.push_back(fd);
    });

    pool.Release(1, 100);
    pool.Release(2, 200);
    pool.Release(3, 300);

    pool.Clear();
    EXPECT_EQ(pool.TotalIdleCount(), 0u);
    EXPECT_EQ(closed_fds.size(), 3u);
}

TEST(BackendPoolHealthStatePurge, DestructorClosesRemainingFds) {
    std::vector<int> closed_fds;
    {
        BackendConnectionPool pool({}, [&](int fd) {
            closed_fds.push_back(fd);
        });
        pool.Release(1, 555);
        pool.Release(2, 777);
    }
    // Out of scope
    EXPECT_EQ(closed_fds.size(), 2u);
    EXPECT_TRUE(std::find(closed_fds.begin(), closed_fds.end(), 555) != closed_fds.end());
    EXPECT_TRUE(std::find(closed_fds.begin(), closed_fds.end(), 777) != closed_fds.end());
}

TEST(BackendPoolHealthStatePurge, MoveConstructionAndAssignment) {
    std::vector<int> closed_fds;
    {
        BackendConnectionPool p1({}, [&](int fd) {
            closed_fds.push_back(fd);
        });
        p1.Release(1, 10);
        EXPECT_EQ(p1.TotalIdleCount(), 1u);

        // Move construct p2 from p1
        BackendConnectionPool p2(std::move(p1));
        EXPECT_EQ(p1.TotalIdleCount(), 0u);
        EXPECT_EQ(p2.TotalIdleCount(), 1u);

        auto acq = p2.Acquire(1);
        ASSERT_TRUE(acq.has_value());
        EXPECT_EQ(*acq, 10);

        // Move assign
        p2.Release(2, 20);
        BackendConnectionPool p3({}, [&](int fd) {
            closed_fds.push_back(fd);
        });
        p3 = std::move(p2);
        EXPECT_EQ(p3.TotalIdleCount(), 1u);
        auto acq3 = p3.Acquire(2);
        ASSERT_TRUE(acq3.has_value());
        EXPECT_EQ(*acq3, 20);
    }
    EXPECT_TRUE(closed_fds.empty());
}

TEST(BackendPoolHealthStatePurge, RealOsSocketClosureVerification) {
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    int client_fd = sv[0];
    int backend_fd = sv[1];

    EXPECT_TRUE(IsFdOpen(backend_fd));

    {
        // Default closer uses real ::close(fd)
        BackendConnectionPool pool;
        pool.Release(1, backend_fd);
        EXPECT_EQ(pool.TotalIdleCount(), 1u);
        // Exiting scope will destruct pool and close backend_fd
    }

    // backend_fd should now be closed by the pool destructor
    EXPECT_FALSE(IsFdOpen(backend_fd));

    // Clean up client_fd
    ::close(client_fd);
}
