#include <radray/allocator.h>

#include <random>

#include <gtest/gtest.h>

namespace radray {
namespace {

struct Range {
    size_t Start;
    size_t Length;
};

bool Overlaps(Range a, Range b) noexcept {
    return a.Start < b.Start + b.Length && b.Start < a.Start + a.Length;
}

TEST(BuddyAllocatorTest, RejectsZeroAndOversizedRequests) {
    BuddyAllocator allocator{1024};

    EXPECT_FALSE(allocator.Allocate(0).has_value());
    EXPECT_FALSE(allocator.Allocate(1025).has_value());
}

TEST(BuddyAllocatorTest, WholeCapacityCanBeAllocatedAgainAfterDestroy) {
    BuddyAllocator allocator{1024};
    auto whole = allocator.Allocate(1024);
    ASSERT_TRUE(whole.has_value());
    EXPECT_EQ(whole->Offset, 0u);

    allocator.Destroy(*whole);

    EXPECT_TRUE(allocator.Allocate(1024).has_value());
}

TEST(BuddyAllocatorTest, BlockOffsetIsAlignedToRoundedSize) {
    BuddyAllocator allocator{1024};

    auto one = allocator.Allocate(1);
    auto three = allocator.Allocate(3);
    auto twelve = allocator.Allocate(12);

    ASSERT_TRUE(one && three && twelve);
    EXPECT_EQ(three->Offset % 4, 0u);
    EXPECT_EQ(twelve->Offset % 16, 0u);
}

TEST(BuddyAllocatorTest, FullHeapRejectsFurtherRequests) {
    BuddyAllocator allocator{1024};
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(allocator.Allocate(256).has_value());
    }

    EXPECT_FALSE(allocator.Allocate(1).has_value());
}

TEST(BuddyAllocatorTest, OnlyFreedBuddiesMergeIntoLargerBlock) {
    BuddyAllocator allocator{1024};
    auto a = allocator.Allocate(256);
    auto b = allocator.Allocate(256);
    auto c = allocator.Allocate(256);
    ASSERT_TRUE(a && b && c && allocator.Allocate(256));

    allocator.Destroy(*b);
    allocator.Destroy(*c);
    EXPECT_FALSE(allocator.Allocate(512).has_value());

    allocator.Destroy(*a);
    auto merged = allocator.Allocate(512);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(merged->Offset, 0u);
}

TEST(BuddyAllocatorTest, NonPowerOfTwoCapacityIsFullyUsable) {
    BuddyAllocator allocator{100};

    auto a = allocator.Allocate(64);
    auto b = allocator.Allocate(32);
    auto c = allocator.Allocate(4);

    ASSERT_TRUE(a && b && c);
    EXPECT_EQ(a->Offset, 0u);
    EXPECT_EQ(b->Offset, 64u);
    EXPECT_EQ(c->Offset, 96u);
    EXPECT_FALSE(allocator.Allocate(1).has_value());
    EXPECT_FALSE(BuddyAllocator{100}.Allocate(101).has_value());
}

TEST(BuddyAllocatorTest, RandomWorkloadKeepsBlocksDisjointAndRecoversCapacity) {
    constexpr size_t kCapacity = 1 << 20;
    BuddyAllocator allocator{kCapacity};
    vector<std::pair<BuddyAllocator::Allocation, Range>> live;
    std::mt19937 rng{42};

    for (int i = 0; i < 2000; ++i) {
        if (live.empty() || std::uniform_int_distribution<int>{0, 2}(rng) != 0) {
            const size_t size = std::uniform_int_distribution<size_t>{1, 4096}(rng);
            auto block = allocator.Allocate(size);
            if (!block) continue;
            const Range range{block->Offset, size};
            ASSERT_LE(range.Start + range.Length, kCapacity);
            for (const auto& [_, other] : live) ASSERT_FALSE(Overlaps(range, other));
            live.emplace_back(*block, range);
        } else {
            const size_t index = std::uniform_int_distribution<size_t>{0, live.size() - 1}(rng);
            allocator.Destroy(live[index].first);
            live.erase(live.begin() + static_cast<ptrdiff_t>(index));
        }
    }
    for (const auto& [block, _] : live) allocator.Destroy(block);

    EXPECT_TRUE(allocator.Allocate(kCapacity).has_value());
}

TEST(FirstFitAllocatorTest, RejectsZeroAndOversizedRequests) {
    FirstFitAllocator allocator{8};

    EXPECT_FALSE(allocator.Allocate(0).has_value());
    EXPECT_FALSE(allocator.Allocate(9).has_value());
    EXPECT_FALSE(FirstFitAllocator{0}.Allocate(1).has_value());
}

TEST(FirstFitAllocatorTest, AllocatesConsecutiveRangesUntilFull) {
    FirstFitAllocator allocator{3};

    auto a = allocator.Allocate(1);
    auto b = allocator.Allocate(2);

    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->Start, 0u);
    EXPECT_EQ(a->Length, 1u);
    EXPECT_EQ(b->Start, 1u);
    EXPECT_EQ(b->Length, 2u);
    EXPECT_FALSE(allocator.Allocate(1).has_value());
}

TEST(FirstFitAllocatorTest, PicksLowestAddressFreeRange) {
    FirstFitAllocator allocator{12};
    FirstFitAllocator::Allocation blocks[6];
    for (auto& block : blocks) block = allocator.Allocate(2).value();

    allocator.Destroy(blocks[3]);
    allocator.Destroy(blocks[1]);
    auto reused = allocator.Allocate(2);

    ASSERT_TRUE(reused.has_value());
    EXPECT_EQ(reused->Start, blocks[1].Start);
}

TEST(FirstFitAllocatorTest, SplitFreeRangeKeepsRemainderAvailable) {
    FirstFitAllocator allocator{16};
    FirstFitAllocator::Allocation blocks[4];
    for (auto& block : blocks) block = allocator.Allocate(4).value();
    allocator.Destroy(blocks[3]);
    allocator.Destroy(blocks[1]);

    auto first = allocator.Allocate(3);
    auto second = allocator.Allocate(1);

    ASSERT_TRUE(first && second);
    EXPECT_EQ(first->Start, 4u);
    EXPECT_EQ(second->Start, 7u);
}

TEST(FirstFitAllocatorTest, DestroyMergesWithBothNeighbours) {
    FirstFitAllocator allocator{10};
    auto a = allocator.Allocate(2).value();
    auto b = allocator.Allocate(3).value();
    auto c = allocator.Allocate(2).value();
    auto d = allocator.Allocate(3).value();

    allocator.Destroy(b);
    allocator.Destroy(d);
    allocator.Destroy(c);
    auto merged = allocator.Allocate(8);

    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(merged->Start, 2u);
    allocator.Destroy(a);
    allocator.Destroy(*merged);
    EXPECT_TRUE(allocator.Allocate(10).has_value());
}

TEST(FirstFitAllocatorTest, RandomWorkloadKeepsRangesDisjointAndRecoversCapacity) {
    constexpr size_t kCapacity = 4096;
    FirstFitAllocator allocator{kCapacity};
    vector<FirstFitAllocator::Allocation> live;
    std::mt19937 rng{42};

    for (int i = 0; i < 5000; ++i) {
        if (live.empty() || std::uniform_int_distribution<int>{0, 2}(rng) != 0) {
            auto block = allocator.Allocate(std::uniform_int_distribution<size_t>{1, 128}(rng));
            if (!block) continue;
            ASSERT_LE(block->Start + block->Length, kCapacity);
            for (const auto& other : live) {
                ASSERT_FALSE(Overlaps({block->Start, block->Length}, {other.Start, other.Length}));
            }
            live.push_back(*block);
        } else {
            const size_t index = std::uniform_int_distribution<size_t>{0, live.size() - 1}(rng);
            allocator.Destroy(live[index]);
            live.erase(live.begin() + static_cast<ptrdiff_t>(index));
        }
    }
    for (const auto& block : live) allocator.Destroy(block);

    auto whole = allocator.Allocate(kCapacity);
    ASSERT_TRUE(whole.has_value());
    EXPECT_EQ(whole->Start, 0u);
}

TEST(BestFitAllocator, RejectsEmptyAndOversizedAllocations) {
    BestFitAllocator allocator{8};
    EXPECT_FALSE(allocator.Allocate(0));
    EXPECT_FALSE(allocator.Allocate(9));
    const auto allocation = allocator.Allocate(8);
    ASSERT_TRUE(allocation);
    EXPECT_EQ(allocation->Offset, 0u);
    EXPECT_EQ(allocation->Count, 8u);
    EXPECT_FALSE(allocator.Allocate(1));
    allocator.Destroy(*allocation);
    EXPECT_EQ(allocator.GetFreeCount(), 8u);
}

TEST(BestFitAllocator, UsesSmallestFittingHoleAndMergesBothNeighbors) {
    BestFitAllocator allocator{24};
    const auto a = allocator.Allocate(8);
    const auto b = allocator.Allocate(4);
    const auto c = allocator.Allocate(6);
    const auto d = allocator.Allocate(6);
    ASSERT_TRUE(a && b && c && d);
    allocator.Destroy(*a);
    allocator.Destroy(*c);
    const auto best = allocator.Allocate(5);
    ASSERT_TRUE(best);
    EXPECT_EQ(best->Offset, c->Offset);
    allocator.Destroy(*best);
    allocator.Destroy(*b);
    const auto merged = allocator.Allocate(18);
    ASSERT_TRUE(merged);
    EXPECT_EQ(merged->Offset, 0u);
    allocator.Destroy(*d);
    allocator.Destroy(*merged);
    EXPECT_TRUE(allocator.Allocate(24));
}

TEST(BestFitAllocator, FragmentedReuseNeverOverlapsLiveRanges) {
    BestFitAllocator allocator{97};
    array<bool, 97> occupied{};
    vector<BestFitAllocator::Allocation> live;
    std::mt19937 random{42};
    for (uint32_t step = 0; step < 3000; ++step) {
        if (!live.empty() && random() % 3 == 0) {
            const auto index = random() % live.size();
            const auto allocation = live[index];
            for (uint32_t i = allocation.Offset; i < allocation.Offset + allocation.Count; ++i) occupied[i] = false;
            allocator.Destroy(allocation);
            live.erase(live.begin() + index);
        } else if (const auto allocation = allocator.Allocate(1 + random() % 12)) {
            for (uint32_t i = allocation->Offset; i < allocation->Offset + allocation->Count; ++i) {
                ASSERT_LT(i, occupied.size());
                ASSERT_FALSE(occupied[i]);
                occupied[i] = true;
            }
            live.push_back(*allocation);
        }
    }
    for (const auto allocation : live) allocator.Destroy(allocation);
    EXPECT_EQ(allocator.GetFreeCount(), 97u);
    EXPECT_TRUE(allocator.Allocate(97));
}

}  // namespace
}  // namespace radray
