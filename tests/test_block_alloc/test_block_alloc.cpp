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
                size_t basicSize) noexcept
                : BlockAllocator(basicSize),
          Counter(counter) {}

    ~TestAllocator() noexcept override = default;

    unique_ptr<Heap> CreateHeap(size_t size) noexcept {
        return make_unique<Heap>(Counter, size);
    }

    BuddyAllocator CreateSubAllocator(size_t size) noexcept { return BuddyAllocator{size}; }

    int* Counter;
};

class HeapInternal2 {
public:
    HeapInternal2(int* counter, size_t size) noexcept : Size(size), Counter(counter) { (*Counter)++; }
    ~HeapInternal2() noexcept { (*Counter)--; }

    HeapInternal2(const HeapInternal2&) = delete;
    HeapInternal2& operator=(const HeapInternal2&) = delete;

    size_t Size;
    int* Counter;
};

class Heap2 {
public:
    Heap2(int* counter, size_t size) noexcept : Size(size), Internal{make_unique<HeapInternal2>(counter, size)} {}

    size_t Size;
    unique_ptr<HeapInternal2> Internal;
};

class TestPagedAllocator : public BlockAllocator<BuddyAllocator, Heap2, TestPagedAllocator> {
public:
    TestPagedAllocator(int* counter, size_t basicPageSize) noexcept
        : BlockAllocator(basicPageSize), Counter(counter) {}

    ~TestPagedAllocator() noexcept override = default;

    unique_ptr<Heap2> CreateHeap(size_t size) noexcept {
        return make_unique<Heap2>(Counter, size);
    }

    BuddyAllocator CreateSubAllocator(size_t size) noexcept { return BuddyAllocator{size}; }

    int* Counter;
};

TEST(Core_Allocator_BlockAllocator, ReleasesEmptyPage) {
    unique_ptr<int> counter = make_unique<int>(0);

    TestPagedAllocator alloc{counter.get(), 4};

    auto a = alloc.Allocate(2);
    ASSERT_TRUE(a.has_value());
    ASSERT_EQ(*counter, 1);

    auto b = alloc.Allocate(2);
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(a->Heap, b->Heap);
    ASSERT_EQ(*counter, 1);

    alloc.Destroy(a.value());
    ASSERT_EQ(*counter, 1);

    alloc.Destroy(b.value());
    ASSERT_EQ(*counter, 0);
}

TEST(Core_Allocator_BlockAllocator, KeepsNonEmptyPagesAlive) {
    unique_ptr<int> counter = make_unique<int>(0);

    TestPagedAllocator alloc{counter.get(), 2};

    auto a = alloc.Allocate(1);
    auto b = alloc.Allocate(1);
    auto c = alloc.Allocate(1);

    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());

    ASSERT_EQ(*counter, 2);
    ASSERT_EQ(a->Heap, b->Heap);
    ASSERT_NE(a->Heap, c->Heap);

    alloc.Destroy(c.value());
    ASSERT_EQ(*counter, 1);

    alloc.Destroy(a.value());
    ASSERT_EQ(*counter, 1);

    alloc.Destroy(b.value());
    ASSERT_EQ(*counter, 0);
}

TEST(Core_Allocator_BlockAllocator, AllocateExceedingBasicPageSize) {
    unique_ptr<int> counter = make_unique<int>(0);

    TestPagedAllocator alloc{counter.get(), 4};

    auto a = alloc.Allocate(5);
    ASSERT_TRUE(a.has_value());
    ASSERT_EQ(*counter, 1);

    alloc.Destroy(a.value());
    ASSERT_EQ(*counter, 0);
}
