#include <gtest/gtest.h>

#include <radray/logger.h>
#include <radray/allocator.h>

using namespace radray;

TEST(Core_Allocator_Buddy, AllocateWithinCapacity) {
    BuddyAllocator allocator(8);  // Capacity of 8 bytes.

    auto alloc1 = allocator.Allocate(4);
    ASSERT_TRUE(alloc1.has_value());
    EXPECT_EQ(alloc1.value(), 0);

    auto alloc2 = allocator.Allocate(2);
    ASSERT_TRUE(alloc2.has_value());
    EXPECT_EQ(alloc2.value(), 4);

    auto alloc3 = allocator.Allocate(2);
    ASSERT_TRUE(alloc3.has_value());
    EXPECT_EQ(alloc3.value(), 6);
}

TEST(Core_Allocator_Buddy, AllocateExceedingCapacity) {
    BuddyAllocator allocator(8);  // Capacity of 8 bytes.

    auto alloc1 = allocator.Allocate(16);  // Request exceeds capacity.
    EXPECT_FALSE(alloc1.has_value());
}

TEST(Core_Allocator_Buddy, AllocateNonPowerOfTwo) {
    BuddyAllocator allocator(8);  // Capacity of 8 bytes.

    auto alloc1 = allocator.Allocate(3);  // Should round up to 4.
    ASSERT_TRUE(alloc1.has_value());
    EXPECT_EQ(alloc1.value(), 0);

    auto alloc2 = allocator.Allocate(1);  // Should round up to 2.
    ASSERT_TRUE(alloc2.has_value());
    EXPECT_EQ(alloc2.value(), 4);

    auto alloc3 = allocator.Allocate(1);
    ASSERT_TRUE(alloc3.has_value());
    EXPECT_EQ(alloc3.value(), 5);
}

TEST(Core_Allocator_Buddy, DeallocateAndReuse) {
    BuddyAllocator allocator(8);  // Capacity of 8 bytes.

    auto alloc1 = allocator.Allocate(4);
    ASSERT_TRUE(alloc1.has_value());
    EXPECT_EQ(alloc1.value(), 0);

    allocator.Destroy(alloc1.value());  // Free the first allocation.

    auto alloc2 = allocator.Allocate(4);  // Reallocate the same size.
    ASSERT_TRUE(alloc2.has_value());
    EXPECT_EQ(alloc2.value(), 0);  // Should reuse the same block.
}

TEST(Core_Allocator_Buddy, ZeroSizeAllocation) {
    BuddyAllocator allocator(8);  // Capacity of 8 bytes.

    auto alloc1 = allocator.Allocate(0);  // Zero-size allocation.
    ASSERT_TRUE(alloc1.has_value());
    EXPECT_EQ(alloc1.value(), 0);  // Should allocate the smallest block.
}

TEST(Core_Allocator_Buddy, FullCapacityAllocation) {
    BuddyAllocator allocator(8);  // Capacity of 8 bytes.

    auto alloc1 = allocator.Allocate(8);  // Allocate the entire capacity.
    ASSERT_TRUE(alloc1.has_value());
    EXPECT_EQ(alloc1.value(), 0);

    auto alloc2 = allocator.Allocate(1);  // No space left for another allocation.
    EXPECT_FALSE(alloc2.has_value());
}

TEST(Core_Allocator_Buddy, DeallocateAndSplitReuse) {
    BuddyAllocator allocator(8);  // Capacity of 8 bytes.

    auto alloc1 = allocator.Allocate(4);
    ASSERT_TRUE(alloc1.has_value());
    EXPECT_EQ(alloc1.value(), 0);

    auto alloc2 = allocator.Allocate(4);
    ASSERT_TRUE(alloc2.has_value());
    EXPECT_EQ(alloc2.value(), 4);

    allocator.Destroy(alloc1.value());  // Free the first allocation.

    auto alloc3 = allocator.Allocate(2);  // Allocate a smaller block.
    ASSERT_TRUE(alloc3.has_value());
    EXPECT_EQ(alloc3.value(), 0);  // Should reuse the first block.
}

TEST(Core_Allocator_Buddy, A) {
    BuddyAllocator buddy{8};
    auto a = buddy.Allocate(4);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a.value(), 0);

    buddy.Destroy(a.value());

    auto b = buddy.Allocate(2);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b.value(), 0);

    auto c = buddy.Allocate(2);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c.value(), 2);

    auto d = buddy.Allocate(4);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d.value(), 4);

    buddy.Destroy(c.value());

    auto e = buddy.Allocate(1);
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e.value(), 2);

    auto f = buddy.Allocate(1);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f.value(), 3);
}

TEST(Core_Allocator_Buddy, AllocateMultipleNonPowerOfTwo) {
    BuddyAllocator allocator(16);  // Capacity of 16 bytes.

    auto alloc1 = allocator.Allocate(3);  // Should round up to 4.
    ASSERT_TRUE(alloc1.has_value());
    EXPECT_EQ(alloc1.value(), 0);

    auto alloc2 = allocator.Allocate(5);  // Should round up to 8.
    ASSERT_TRUE(alloc2.has_value());
    EXPECT_EQ(alloc2.value(), 8);

    auto alloc3 = allocator.Allocate(5);  // Should round up to 8.
    EXPECT_FALSE(alloc3.has_value());     // No space left for another allocation.

    auto alloc4 = allocator.Allocate(1);
    ASSERT_TRUE(alloc4.has_value());
    EXPECT_EQ(alloc4.value(), 4);
}

TEST(Core_Allocator_Buddy, AllocateAndDeallocateNonPowerOfTwo) {
    BuddyAllocator allocator(16);  // Capacity of 16 bytes.

    auto alloc1 = allocator.Allocate(3);  // Should round up to 4.
    ASSERT_TRUE(alloc1.has_value());
    EXPECT_EQ(alloc1.value(), 0);

    auto alloc2 = allocator.Allocate(5);  // Should round up to 8.
    ASSERT_TRUE(alloc2.has_value());
    EXPECT_EQ(alloc2.value(), 8);

    allocator.Destroy(alloc1.value());  // Free the first allocation.

    auto alloc3 = allocator.Allocate(2);  // Should round up to 2.
    ASSERT_TRUE(alloc3.has_value());
    EXPECT_EQ(alloc3.value(), 0);  // Should reuse the first block.

    auto alloc4 = allocator.Allocate(1);  // Should round up to 2.
    ASSERT_TRUE(alloc4.has_value());
    EXPECT_EQ(alloc4.value(), 2);
}

#if GTEST_HAS_DEATH_TEST
#if !defined(NDEBUG)

TEST(Core_Allocator_Buddy_Death, DestroyInvalidOffsetAsserts) {
    BuddyAllocator allocator(8);
    ASSERT_DEATH({
        allocator.Destroy(999);
    }, ".*");
}

TEST(Core_Allocator_Buddy_Death, DoubleFreeAsserts) {
    BuddyAllocator allocator(8);
    auto a = allocator.Allocate(1);
    ASSERT_TRUE(a.has_value());
    allocator.Destroy(a.value());
    ASSERT_DEATH({
        allocator.Destroy(a.value());
    }, ".*");
}

#endif  // !defined(NDEBUG)
#endif  // GTEST_HAS_DEATH_TEST
