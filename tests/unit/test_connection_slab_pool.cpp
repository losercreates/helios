#include <gtest/gtest.h>
#include "net/connection_slab_pool.hpp"

#include <string>
#include <vector>

using namespace helios;

namespace {

struct TestNode {
    int id{0};
    std::string name;
    static inline int live_instances{0};

    TestNode() : id(0), name("default") {
        live_instances++;
    }

    TestNode(int id_val, std::string name_val) : id(id_val), name(std::move(name_val)) {
        live_instances++;
    }

    ~TestNode() {
        live_instances--;
    }
};

} // namespace

TEST(ConnectionSlabPoolTest, BasicAcquireAndRelease) {
    TestNode::live_instances = 0;
    {
        ConnectionSlabPool<TestNode, 16> pool(1);
        EXPECT_EQ(pool.ActiveCount(), 0u);
        EXPECT_EQ(pool.TotalCapacity(), 16u);

        TestNode* n1 = pool.Acquire(101, "node1");
        ASSERT_NE(n1, nullptr);
        EXPECT_EQ(n1->id, 101);
        EXPECT_EQ(n1->name, "node1");
        EXPECT_EQ(pool.ActiveCount(), 1u);
        EXPECT_EQ(TestNode::live_instances, 1);

        TestNode* n2 = pool.Acquire(102, "node2");
        ASSERT_NE(n2, nullptr);
        EXPECT_EQ(n2->id, 102);
        EXPECT_EQ(pool.ActiveCount(), 2u);
        EXPECT_EQ(TestNode::live_instances, 2);

        pool.Release(n1);
        EXPECT_EQ(pool.ActiveCount(), 1u);
        EXPECT_EQ(TestNode::live_instances, 1);

        // Re-acquiring should reuse released slot
        TestNode* n3 = pool.Acquire(103, "node3");
        ASSERT_NE(n3, nullptr);
        EXPECT_EQ(n3->id, 103);
        EXPECT_EQ(pool.ActiveCount(), 2u);
        EXPECT_EQ(TestNode::live_instances, 2);

        pool.Release(n2);
        pool.Release(n3);
        EXPECT_EQ(pool.ActiveCount(), 0u);
        EXPECT_EQ(TestNode::live_instances, 0);
    }
    EXPECT_EQ(TestNode::live_instances, 0);
}

TEST(ConnectionSlabPoolTest, DynamicChunkGrowth) {
    TestNode::live_instances = 0;
    {
        ConnectionSlabPool<TestNode, 4> pool(1);
        EXPECT_EQ(pool.TotalCapacity(), 4u);

        std::vector<TestNode*> nodes;
        for (int i = 0; i < 10; ++i) {
            nodes.push_back(pool.Acquire(i, "item_" + std::to_string(i)));
        }

        EXPECT_EQ(pool.ActiveCount(), 10u);
        EXPECT_GE(pool.TotalCapacity(), 12u); // At least 3 chunks allocated (3 * 4 = 12)
        EXPECT_EQ(TestNode::live_instances, 10);

        for (auto* n : nodes) {
            pool.Release(n);
        }

        EXPECT_EQ(pool.ActiveCount(), 0u);
        EXPECT_EQ(TestNode::live_instances, 0);
    }
    EXPECT_EQ(TestNode::live_instances, 0);
}

TEST(ConnectionSlabPoolTest, DestructorCleansUpUnreleasedSlots) {
    TestNode::live_instances = 0;
    {
        ConnectionSlabPool<TestNode, 8> pool(1);
        pool.Acquire(1, "leak_test_1");
        pool.Acquire(2, "leak_test_2");
        EXPECT_EQ(TestNode::live_instances, 2);
    }
    // Pool destructor should have called destructors on unreleased slots
    EXPECT_EQ(TestNode::live_instances, 0);
}
