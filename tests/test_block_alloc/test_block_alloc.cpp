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

class TestFreeListPagedAllocator : public BlockAllocator<FreeListAllocator, Heap2, TestFreeListPagedAllocator> {
public:
    TestFreeListPagedAllocator(int* counter, size_t basicPageSize) noexcept
        : BlockAllocator(basicPageSize), Counter(counter) {}

    ~TestFreeListPagedAllocator() noexcept override = default;

    unique_ptr<Heap2> CreateHeap(size_t size) noexcept {
        return make_unique<Heap2>(Counter, size);
    }

    FreeListAllocator CreateSubAllocator(size_t size) noexcept { return FreeListAllocator{size}; }

    int* Counter;
};

TEST(Core_Allocator_BlockAllocator, AllocateZeroReturnsNullopt) {
    unique_ptr<int> counter = make_unique<int>(0);

    TestPagedAllocator alloc{counter.get(), 4};
    auto a = alloc.Allocate(0);
    ASSERT_FALSE(a.has_value());
    ASSERT_EQ(*counter, 0);
}

TEST(Core_Allocator_BlockAllocator, BuddyRoundingUpStillCorrect) {
    unique_ptr<int> counter = make_unique<int>(0);

    // BuddyAllocator 会按 2^n 取整；BlockAllocator 的 freeBytes 统计是“请求字节数”，
    // 这会导致 freeBytes 可能大于实际可分配空间。这里验证即便如此也不会错误复用同一页。
    TestPagedAllocator alloc{counter.get(), 4};

    auto a = alloc.Allocate(3);
    ASSERT_TRUE(a.has_value());
    ASSERT_EQ(*counter, 1);

    auto b = alloc.Allocate(1);
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(*counter, 2);
    ASSERT_NE(a->Heap, b->Heap);

    alloc.Destroy(a.value());
    alloc.Destroy(b.value());
    ASSERT_EQ(*counter, 0);
}

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

TEST(Core_Allocator_BlockAllocator, FullPageForcesNewPage_FreeList) {
    unique_ptr<int> counter = make_unique<int>(0);

    TestFreeListPagedAllocator alloc{counter.get(), 4};

    auto a = alloc.Allocate(2);
    auto b = alloc.Allocate(2);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(*counter, 1);
    ASSERT_EQ(a->Heap, b->Heap);

    // 第一页已满，下一次必须新建页。
    auto c = alloc.Allocate(1);
    ASSERT_TRUE(c.has_value());
    ASSERT_EQ(*counter, 2);
    ASSERT_NE(c->Heap, a->Heap);

    alloc.Destroy(a.value());
    alloc.Destroy(b.value());
    alloc.Destroy(c.value());
    ASSERT_EQ(*counter, 0);
}

TEST(Core_Allocator_BlockAllocator, FragmentationFallsBackToNewPage_FreeList) {
    unique_ptr<int> counter = make_unique<int>(0);

    TestFreeListPagedAllocator alloc{counter.get(), 6};

    auto a = alloc.Allocate(2);
    auto b = alloc.Allocate(2);
    auto c = alloc.Allocate(2);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());
    ASSERT_EQ(*counter, 1);
    ASSERT_EQ(a->Heap, b->Heap);
    ASSERT_EQ(b->Heap, c->Heap);

    // 释放首尾块，形成 2 + 2 的碎片：freeBytes(=4) >= 3，但子分配器无法给出连续 3。
    alloc.Destroy(a.value());
    alloc.Destroy(c.value());
    ASSERT_EQ(*counter, 1);

    auto d = alloc.Allocate(3);
    ASSERT_TRUE(d.has_value());
    ASSERT_EQ(*counter, 2);
    ASSERT_NE(d->Heap, b->Heap);

    alloc.Destroy(b.value());
    alloc.Destroy(d.value());
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

TEST(Core_Allocator_BlockAllocator, ManySmallAllocationsSpillToNewPage_FreeList) {
    unique_ptr<int> counter = make_unique<int>(0);

    TestFreeListPagedAllocator alloc{counter.get(), 8};

    radray::vector<BlockAllocation<Heap2>> allocs;
    allocs.reserve(9);

    for (int i = 0; i < 8; ++i) {
        auto a = alloc.Allocate(1);
        ASSERT_TRUE(a.has_value());
        allocs.push_back(a.value());
    }
    ASSERT_EQ(*counter, 1);
    for (int i = 1; i < 8; ++i) {
        ASSERT_EQ(allocs[0].Heap, allocs[i].Heap);
    }

    auto ninth = alloc.Allocate(1);
    ASSERT_TRUE(ninth.has_value());
    ASSERT_EQ(*counter, 2);
    ASSERT_NE(ninth->Heap, allocs[0].Heap);

    for (auto& x : allocs) {
        alloc.Destroy(x);
    }
    alloc.Destroy(ninth.value());
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

#if GTEST_HAS_DEATH_TEST

#if !defined(NDEBUG)
TEST(Core_Allocator_BlockAllocator_Death, DestroyNullHeapAsserts) {
    unique_ptr<int> counter = make_unique<int>(0);
    TestPagedAllocator alloc{counter.get(), 4};

    BlockAllocation<Heap2> bogus{nullptr, 0, 1};
    ASSERT_DEATH({
        alloc.Destroy(bogus);
    }, ".*");
}

TEST(Core_Allocator_BlockAllocator_Death, DestroyForeignHeapAsserts) {
    unique_ptr<int> counter = make_unique<int>(0);
    TestPagedAllocator allocA{counter.get(), 4};
    TestPagedAllocator allocB{counter.get(), 4};

    auto a = allocA.Allocate(1);
    ASSERT_TRUE(a.has_value());

    // heap 属于 allocA，交给 allocB Destroy 应触发断言。
    ASSERT_DEATH({
        allocB.Destroy(a.value());
    }, ".*");

    // 清理 allocA，避免泄漏（这一行不应该走到，但写上更清晰）。
}
#endif  // !defined(NDEBUG)

TEST(Core_Allocator_BlockAllocator_Death, DoubleFreeAborts_Buddy) {
    unique_ptr<int> counter = make_unique<int>(0);
    TestPagedAllocator alloc{counter.get(), 8};

    auto a = alloc.Allocate(1);
    auto b = alloc.Allocate(1);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(a->Heap, b->Heap);  // 确保页仍存活

    alloc.Destroy(a.value());
    ASSERT_DEATH({
        alloc.Destroy(a.value());
    }, ".*");
}

TEST(Core_Allocator_BlockAllocator_Death, DoubleFreeAborts_FreeList) {
    unique_ptr<int> counter = make_unique<int>(0);
    TestFreeListPagedAllocator alloc{counter.get(), 8};

    auto a = alloc.Allocate(2);
    auto b = alloc.Allocate(2);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(a->Heap, b->Heap);

    alloc.Destroy(a.value());
    ASSERT_DEATH({
        alloc.Destroy(a.value());
    }, ".*");
}

TEST(Core_Allocator_BlockAllocator_Death, InvalidOffsetAborts_Buddy) {
    unique_ptr<int> counter = make_unique<int>(0);
    TestPagedAllocator alloc{counter.get(), 4};

    auto live = alloc.Allocate(1);
    ASSERT_TRUE(live.has_value());

    // 伪造一个 start 越界的 allocation，但 heap 指针是真实存在的。
    BlockAllocation<Heap2> bogus{live->Heap, live->Heap->Size + 1, 1};
    ASSERT_DEATH({
        alloc.Destroy(bogus);
    }, ".*");
}

TEST(Core_Allocator_BlockAllocator_Death, InvalidOffsetAborts_FreeList) {
    unique_ptr<int> counter = make_unique<int>(0);
    TestFreeListPagedAllocator alloc{counter.get(), 4};

    auto live = alloc.Allocate(1);
    ASSERT_TRUE(live.has_value());

    BlockAllocation<Heap2> bogus{live->Heap, live->Heap->Size + 123, 1};
    ASSERT_DEATH({
        alloc.Destroy(bogus);
    }, ".*");
}

#endif  // GTEST_HAS_DEATH_TEST
