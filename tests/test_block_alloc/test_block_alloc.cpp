#include <gtest/gtest.h>

#include <radray/logger.h>
#include <radray/allocator.h>

using namespace radray;
using namespace std;

class HeapInternal {
public:
    HeapInternal(int* counter, size_t size) noexcept : Size(size), Counter(counter) {
        // RADRAY_INFO_LOG("HeapInternal created with size: {}", size);
        (*Counter)++;
    }

    ~HeapInternal() noexcept {
        // RADRAY_INFO_LOG("HeapInternal destroyed with size: {}", Size);
        (*Counter)--;
    }

    HeapInternal(const HeapInternal&) = delete;
    HeapInternal& operator=(const HeapInternal&) = delete;

    size_t Size;
    int* Counter;
};

class Heap {
public:
    Heap(int* counter, size_t size) noexcept : Size(size), Internal{make_unique<HeapInternal>(counter, size)} {}

    size_t Size;
    unique_ptr<HeapInternal> Internal;
};

class TestAllocator : public BlockAllocator<BuddyAllocator, Heap, TestAllocator> {
public:
    TestAllocator(
        int* counter,
        size_t basicSize,
        size_t destroyThreshold) noexcept
        : BlockAllocator(basicSize, destroyThreshold),
          Counter(counter) {}

    ~TestAllocator() noexcept override = default;

    unique_ptr<Heap> CreateHeap(size_t size) noexcept {
        return make_unique<Heap>(Counter, size);
    }

    BuddyAllocator CreateSubAllocator(size_t size) noexcept { return BuddyAllocator{size}; }

    int* Counter;
};

TEST(Core_Allocator_BlockAllocator, Basic) {
    unique_ptr<int> counter = make_unique<int>(0);

    TestAllocator alloc{counter.get(), 2, 0};
    auto a = alloc.Allocate(1);
    ASSERT_TRUE(a.has_value());
    ASSERT_EQ(a.value().Length, 1);
    ASSERT_EQ(a.value().Start, 0);

    ASSERT_EQ(*counter, 1);

    auto b = alloc.Allocate(1);
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(b.value().Length, 1);
    ASSERT_EQ(b.value().Start, 1);

    ASSERT_EQ(a.value().Heap, b.value().Heap);

    ASSERT_EQ(*counter, 1);

    auto c = alloc.Allocate(1);
    ASSERT_TRUE(c.has_value());
    ASSERT_EQ(c.value().Length, 1);
    ASSERT_EQ(c.value().Start, 0);

    ASSERT_NE(a.value().Heap, c.value().Heap);

    ASSERT_EQ(*counter, 2);

    auto d = alloc.Allocate(2);
    ASSERT_TRUE(d.has_value());
    ASSERT_EQ(d.value().Length, 2);
    ASSERT_EQ(d.value().Start, 0);

    ASSERT_EQ(*counter, 3);

    alloc.Destroy(c.value());

    ASSERT_EQ(*counter, 2);

    alloc.Destroy(d.value());

    ASSERT_EQ(*counter, 1);
}

TEST(Core_Allocator_BlockAllocator, AllocateAndDestroyMultiple) {
    unique_ptr<int> counter = make_unique<int>(0);

    TestAllocator alloc{counter.get(), 4, 1};

    auto a = alloc.Allocate(2);
    ASSERT_TRUE(a.has_value());
    ASSERT_EQ(a.value().Length, 2);
    ASSERT_EQ(a.value().Start, 0);

    ASSERT_EQ(*counter, 1);

    auto b = alloc.Allocate(2);
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(b.value().Length, 2);
    ASSERT_EQ(b.value().Start, 2);

    ASSERT_EQ(a.value().Heap, b.value().Heap);

    ASSERT_EQ(*counter, 1);

    auto c = alloc.Allocate(4);
    ASSERT_TRUE(c.has_value());
    ASSERT_EQ(c.value().Length, 4);
    ASSERT_EQ(c.value().Start, 0);

    ASSERT_NE(a.value().Heap, c.value().Heap);

    ASSERT_EQ(*counter, 2);

    alloc.Destroy(a.value());
    ASSERT_EQ(*counter, 2);

    alloc.Destroy(b.value());
    ASSERT_EQ(*counter, 2);

    alloc.Destroy(c.value());
    ASSERT_EQ(*counter, 1);
}

TEST(Core_Allocator_BlockAllocator, AllocateExceedingCapacity) {
    unique_ptr<int> counter = make_unique<int>(0);

    TestAllocator alloc{counter.get(), 4, 1};

    auto a = alloc.Allocate(5);
    ASSERT_TRUE(a.has_value());

    ASSERT_EQ(*counter, 1);
}

TEST(Core_Allocator_BlockAllocator, DestroyInvalidAllocation) {
    unique_ptr<int> counter = make_unique<int>(0);

    TestAllocator alloc{counter.get(), 4, 1};

    auto a = alloc.Allocate(2);
    ASSERT_TRUE(a.has_value());
    ASSERT_EQ(*counter, 1);

    BlockAllocation<Heap> invalidAllocation{nullptr, 0, 2};
    ASSERT_DEATH(alloc.Destroy(invalidAllocation), ".*");

    ASSERT_EQ(*counter, 1);

    alloc.Destroy(a.value());
    ASSERT_EQ(*counter, 1);
}

TEST(Core_Allocator_BlockAllocator, ReuseFreedBlocks) {
    unique_ptr<int> counter = make_unique<int>(0);

    TestAllocator alloc{counter.get(), 4, 1};

    auto a = alloc.Allocate(2);
    ASSERT_TRUE(a.has_value());
    ASSERT_EQ(a.value().Length, 2);
    ASSERT_EQ(a.value().Start, 0);

    ASSERT_EQ(*counter, 1);

    alloc.Destroy(a.value());
    ASSERT_EQ(*counter, 1);

    auto b = alloc.Allocate(2);
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(b.value().Length, 2);
    ASSERT_EQ(b.value().Start, 0);

    ASSERT_EQ(*counter, 1);
}
