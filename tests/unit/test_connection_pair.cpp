#include <gtest/gtest.h>
#include "net/connection_pair.hpp"
#include "io/ring_engine.hpp"
#include <chrono>

using namespace helios;

TEST(ConnectionPairUnitTest, ValidStateTransitions) {
    RingEngine engine(64);
    auto conn = std::make_shared<ConnectionPair>(1, engine, 10);

    EXPECT_EQ(conn->GetState(), ConnectionState::Init);

    EXPECT_TRUE(conn->TransitionTo(ConnectionState::ConnectingBackend));
    EXPECT_EQ(conn->GetState(), ConnectionState::ConnectingBackend);

    EXPECT_TRUE(conn->TransitionTo(ConnectionState::Established));
    EXPECT_EQ(conn->GetState(), ConnectionState::Established);

    EXPECT_TRUE(conn->TransitionTo(ConnectionState::HalfClosedClient));
    EXPECT_EQ(conn->GetState(), ConnectionState::HalfClosedClient);

    EXPECT_TRUE(conn->TransitionTo(ConnectionState::PendingCancellation));
    EXPECT_EQ(conn->GetState(), ConnectionState::PendingCancellation);

    EXPECT_TRUE(conn->TransitionTo(ConnectionState::Closed));
    EXPECT_EQ(conn->GetState(), ConnectionState::Closed);
    EXPECT_TRUE(conn->IsClosed());
}

TEST(ConnectionPairUnitTest, InvalidStateTransitionsRejected) {
    RingEngine engine(64);
    auto conn = std::make_shared<ConnectionPair>(1, engine, 10);

    // Cannot jump from Init to HalfClosedClient directly without being Established
    EXPECT_FALSE(conn->TransitionTo(ConnectionState::HalfClosedClient));
    EXPECT_EQ(conn->GetState(), ConnectionState::Init);

    // Closed is terminal state
    conn->TransitionTo(ConnectionState::Closed);
    EXPECT_FALSE(conn->TransitionTo(ConnectionState::Established));
    EXPECT_EQ(conn->GetState(), ConnectionState::Closed);
}

TEST(ConnectionPairUnitTest, PendingCqeDeferredCleanupGate) {
    RingEngine engine(64);
    bool cleanup_called = false;
    auto conn = std::make_shared<ConnectionPair>(1, engine, 10);
    conn->SetCleanupCallback([&](uint64_t id) {
        cleanup_called = true;
        EXPECT_EQ(id, 1u);
    });

    conn->TransitionTo(ConnectionState::Established);

    // Simulate 2 in-flight io_uring operations
    conn->IncrementPendingCqe();
    conn->IncrementPendingCqe();
    EXPECT_EQ(conn->GetPendingCqeCount(), 2u);

    // Initiate close while CQEs are still pending
    conn->InitiateClose("Test close");
    EXPECT_EQ(conn->GetState(), ConnectionState::PendingCancellation);
    EXPECT_FALSE(cleanup_called); // Cleanup must be deferred

    // First CQE reaped
    conn->DecrementPendingCqe();
    EXPECT_EQ(conn->GetPendingCqeCount(), 1u);
    EXPECT_EQ(conn->GetState(), ConnectionState::PendingCancellation);
    EXPECT_FALSE(cleanup_called);

    // Second (last) CQE reaped
    conn->DecrementPendingCqe();
    EXPECT_EQ(conn->GetPendingCqeCount(), 0u);
    EXPECT_EQ(conn->GetState(), ConnectionState::Closed);
    EXPECT_TRUE(cleanup_called); // Cleanup executed now
}

TEST(ConnectionPairUnitTest, HalfCloseFallbackTimeout) {
    RingEngine engine(64);
    auto conn = std::make_shared<ConnectionPair>(1, engine, 10);
    conn->TransitionTo(ConnectionState::Established);

    // Simulate 1 in-flight read operation
    conn->IncrementPendingCqe();
    conn->HandleClientFin();
    EXPECT_EQ(conn->GetState(), ConnectionState::HalfClosedClient);

    auto start_time = ConnectionPair::Clock::now();
    // Timeout check before limit (2 seconds) -> remains HalfClosedClient
    EXPECT_FALSE(conn->CheckHalfCloseTimeout(start_time + std::chrono::seconds(2), std::chrono::seconds(5)));
    EXPECT_EQ(conn->GetState(), ConnectionState::HalfClosedClient);

    // Timeout check after limit (6 seconds) -> transitions to PendingCancellation because pending_cqe > 0
    EXPECT_TRUE(conn->CheckHalfCloseTimeout(start_time + std::chrono::seconds(6), std::chrono::seconds(5)));
    EXPECT_EQ(conn->GetState(), ConnectionState::PendingCancellation);

    // Decrement pending CQE -> transitions to Closed
    conn->DecrementPendingCqe();
    EXPECT_EQ(conn->GetState(), ConnectionState::Closed);
}

TEST(ConnectionPairUnitTest, DuplicateInitiateCloseIdempotent) {
    RingEngine engine(64);
    auto conn = std::make_shared<ConnectionPair>(1, engine, 10);
    conn->TransitionTo(ConnectionState::Established);

    conn->InitiateClose("Close 1");
    EXPECT_EQ(conn->GetState(), ConnectionState::Closed);

    // Repeated call should do nothing and remain Closed
    conn->InitiateClose("Close 2");
    EXPECT_EQ(conn->GetState(), ConnectionState::Closed);
}
