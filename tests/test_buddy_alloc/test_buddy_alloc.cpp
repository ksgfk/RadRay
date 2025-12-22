#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <vector>
#include <random>

#include <radray/logger.h>
#include <radray/allocator.h>

using namespace radray;

TEST(BuddyAllocatorTest, BasicAllocation) {
    BuddyAllocator allocator(1024);
    auto alloc = allocator.Allocate(128);
    ASSERT_TRUE(alloc.has_value());
    EXPECT_EQ(alloc->Offset, 0);
    allocator.Destroy(*alloc);
}

TEST(BuddyAllocatorTest, ZeroAllocation) {
    BuddyAllocator allocator(1024);
    auto alloc = allocator.Allocate(0);
    EXPECT_FALSE(alloc.has_value());
}

TEST(BuddyAllocatorTest, OversizedAllocation) {
    BuddyAllocator allocator(1024);
    auto alloc = allocator.Allocate(1025);
    EXPECT_FALSE(alloc.has_value());
}

TEST(BuddyAllocatorTest, FullCapacityAllocation) {
    BuddyAllocator allocator(1024);
    auto alloc = allocator.Allocate(1024);
    ASSERT_TRUE(alloc.has_value());
    EXPECT_EQ(alloc->Offset, 0);
    allocator.Destroy(*alloc);
    
    // Should be able to allocate again
    alloc = allocator.Allocate(1024);
    ASSERT_TRUE(alloc.has_value());
    allocator.Destroy(*alloc);
}

TEST(BuddyAllocatorTest, NonPowerOfTwoCapacity) {
    size_t capacity = 100;
    BuddyAllocator allocator(capacity);
    
    // Should be able to allocate full capacity
    auto alloc = allocator.Allocate(100);
    ASSERT_TRUE(alloc.has_value());
    allocator.Destroy(*alloc);

    // Should not be able to allocate more
    alloc = allocator.Allocate(101);
    EXPECT_FALSE(alloc.has_value());
}

TEST(BuddyAllocatorTest, NonPowerOfTwoCapacityFragmentation) {
    // Capacity 100. Virtual 128.
    // Root (0-127) -> L(0-63), R(64-127)
    // R(64-127) -> RL(64-95), RR(96-127)
    // RR(96-127) -> RRL(96-111), RRR(112-127)
    // Capacity cuts off at 100.
    // RR(96-127) has actual capacity 4 (96, 97, 98, 99).
    
    BuddyAllocator allocator(100);
    
    // Allocate 64. Should take L(0-63).
    auto a1 = allocator.Allocate(64);
    ASSERT_TRUE(a1.has_value());
    EXPECT_EQ(a1->Offset, 0);
    
    // Allocate 32. Should take RL(64-95).
    auto a2 = allocator.Allocate(32);
    ASSERT_TRUE(a2.has_value());
    EXPECT_EQ(a2->Offset, 64);
    
    // Allocate 4. Should take RRL(96-111) which has capacity 4?
    // Wait, RRL(96-111) size is 16. Actual capacity is min(16, 100-96=4) = 4.
    // If we request 4. Target size 4.
    // It will traverse down to size 4 nodes.
    // RR(96-127) [cap 4] -> RRL(96-111) [cap 4] -> ...
    // Eventually it finds a node of size 4 at offset 96.
    auto a3 = allocator.Allocate(4);
    ASSERT_TRUE(a3.has_value());
    EXPECT_EQ(a3->Offset, 96);
    
    // Now full (64+32+4 = 100).
    auto a4 = allocator.Allocate(1);
    EXPECT_FALSE(a4.has_value());
    
    allocator.Destroy(*a1);
    allocator.Destroy(*a2);
    allocator.Destroy(*a3);
    
    // Should be empty
    auto aFull = allocator.Allocate(100);
    ASSERT_TRUE(aFull.has_value());
    allocator.Destroy(*aFull);
}

TEST(BuddyAllocatorTest, MultipleAllocations) {
    BuddyAllocator allocator(1024);
    std::vector<BuddyAllocator::Allocation> allocs;
    
    for (int i = 0; i < 4; ++i) {
        auto alloc = allocator.Allocate(256);
        ASSERT_TRUE(alloc.has_value());
        allocs.push_back(*alloc);
    }
    
    // Should be full
    auto alloc = allocator.Allocate(1);
    EXPECT_FALSE(alloc.has_value());
    
    // Free all
    for (auto& a : allocs) {
        allocator.Destroy(a);
    }
    
    // Should be empty and merge back
    alloc = allocator.Allocate(1024);
    ASSERT_TRUE(alloc.has_value());
    allocator.Destroy(*alloc);
}

TEST(BuddyAllocatorTest, FragmentationAndCoalescing) {
    BuddyAllocator allocator(1024);
    
    auto a1 = allocator.Allocate(256);
    auto a2 = allocator.Allocate(256);
    auto a3 = allocator.Allocate(256);
    auto a4 = allocator.Allocate(256);
    
    ASSERT_TRUE(a1 && a2 && a3 && a4);
    
    // Free middle ones
    allocator.Destroy(*a2);
    allocator.Destroy(*a3);
    
    // Should NOT be able to allocate 512 because free blocks are not buddies
    auto a5 = allocator.Allocate(512);
    EXPECT_FALSE(a5.has_value());
    
    // Free a1. Now a1(0-255) and a2(256-511) are free. They are buddies.
    allocator.Destroy(*a1);
    
    auto a6 = allocator.Allocate(512);
    ASSERT_TRUE(a6.has_value());
    EXPECT_EQ(a6->Offset, 0);
    
    allocator.Destroy(*a6);
    allocator.Destroy(*a4);
}

TEST(BuddyAllocatorTest, RandomStress) {
    const size_t capacity = 1024 * 1024;
    BuddyAllocator allocator(capacity);
    std::vector<BuddyAllocator::Allocation> allocations;
    std::mt19937 rng(42);
    
    for (int i = 0; i < 2000; ++i) {
        if (allocations.empty() || (std::uniform_int_distribution<>(0, 2)(rng) != 0)) {
            // Allocate (2/3 chance)
            size_t size = std::uniform_int_distribution<size_t>(1, 4096)(rng);
            auto alloc = allocator.Allocate(size);
            if (alloc) {
                allocations.push_back(*alloc);
            }
        } else {
            // Free (1/3 chance)
            size_t index = std::uniform_int_distribution<size_t>(0, allocations.size() - 1)(rng);
            allocator.Destroy(allocations[index]);
            allocations.erase(allocations.begin() + index);
        }
    }
    
    for (auto& alloc : allocations) {
        allocator.Destroy(alloc);
    }
    
    // Should be fully free
    auto finalAlloc = allocator.Allocate(capacity);
    EXPECT_TRUE(finalAlloc.has_value());
    if (finalAlloc) allocator.Destroy(*finalAlloc);
}

TEST(BuddyAllocatorTest, AlignmentCheck) {
    BuddyAllocator allocator(1024);
    // Allocating 1 byte should result in a block of size 1 (if min block size allows)
    // But BuddyAllocator usually has a minimum block size or just powers of 2.
    // The implementation uses bit_ceil(request).
    // bit_ceil(1) = 1.
    // bit_ceil(3) = 4.
    
    auto a1 = allocator.Allocate(1);
    ASSERT_TRUE(a1.has_value());
    // Offset should be aligned to 1.
    
    auto a2 = allocator.Allocate(3);
    ASSERT_TRUE(a2.has_value());
    // Size 4. Offset should be multiple of 4.
    EXPECT_EQ(a2->Offset % 4, 0);
    
    auto a3 = allocator.Allocate(12);
    ASSERT_TRUE(a3.has_value());
    // Size 16. Offset should be multiple of 16.
    EXPECT_EQ(a3->Offset % 16, 0);
    
    allocator.Destroy(*a1);
    allocator.Destroy(*a2);
    allocator.Destroy(*a3);
}
