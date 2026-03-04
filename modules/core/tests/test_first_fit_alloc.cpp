#include <gtest/gtest.h>

#include <random>
#include <vector>

#include <radray/allocator.h>

using namespace radray;

TEST(Core_Allocator_FirstFit, BasicSequential) {
    FirstFitAllocator alloc{2};

    auto a = alloc.Allocate(1);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->Start, 0);
    EXPECT_EQ(a->Length, 1);

    auto b = alloc.Allocate(1);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->Start, 1);
    EXPECT_EQ(b->Length, 1);

    EXPECT_FALSE(alloc.Allocate(1).has_value());
}

TEST(Core_Allocator_FirstFit, ExactFitAndReuse) {
    FirstFitAllocator alloc{8};

    auto a = alloc.Allocate(8);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->Start, 0);
    EXPECT_EQ(a->Length, 8);

    EXPECT_FALSE(alloc.Allocate(1).has_value());

    alloc.Destroy(*a);
    auto b = alloc.Allocate(8);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->Start, 0);
    EXPECT_EQ(b->Length, 8);
}

TEST(Core_Allocator_FirstFit, ZeroCapacityAndZeroSize) {
    FirstFitAllocator empty{0};
    EXPECT_FALSE(empty.Allocate(1).has_value());
    EXPECT_FALSE(empty.Allocate(0).has_value());

    FirstFitAllocator alloc{8};
    EXPECT_FALSE(alloc.Allocate(0).has_value());
}

TEST(Core_Allocator_FirstFit, FirstFitByAddress) {
    FirstFitAllocator alloc{12};

    auto a0 = alloc.Allocate(2).value();
    auto a1 = alloc.Allocate(2).value();
    auto a2 = alloc.Allocate(2).value();
    auto a3 = alloc.Allocate(2).value();
    auto a4 = alloc.Allocate(2).value();
    auto a5 = alloc.Allocate(2).value();
    (void)a0;
    (void)a2;
    (void)a4;
    (void)a5;

    // Free in reverse address order: old implementation could pick Start=6 first.
    alloc.Destroy(a3);  // [6,2]
    alloc.Destroy(a1);  // [2,2]

    auto pick = alloc.Allocate(2);
    ASSERT_TRUE(pick.has_value());
    EXPECT_EQ(pick->Start, 2);
    EXPECT_EQ(pick->Length, 2);
}

TEST(Core_Allocator_FirstFit, CoalesceBothSides) {
    FirstFitAllocator alloc{10};

    auto a = alloc.Allocate(2).value();  // [0,2]
    auto b = alloc.Allocate(3).value();  // [2,3]
    auto c = alloc.Allocate(2).value();  // [5,2]
    auto d = alloc.Allocate(3).value();  // [7,3]

    alloc.Destroy(b);
    alloc.Destroy(d);
    alloc.Destroy(c);

    auto big = alloc.Allocate(8);
    ASSERT_TRUE(big.has_value());
    EXPECT_EQ(big->Start, 2);
    EXPECT_EQ(big->Length, 8);

    alloc.Destroy(a);
    alloc.Destroy(*big);

    auto full = alloc.Allocate(10);
    ASSERT_TRUE(full.has_value());
    EXPECT_EQ(full->Start, 0);
    EXPECT_EQ(full->Length, 10);
}

TEST(Core_Allocator_FirstFit, FragmentationReuseLowerAddressFirst) {
    FirstFitAllocator alloc{16};

    auto a = alloc.Allocate(4).value();  // [0,4]
    auto b = alloc.Allocate(4).value();  // [4,4]
    auto c = alloc.Allocate(4).value();  // [8,4]
    auto d = alloc.Allocate(4).value();  // [12,4]
    (void)a;
    (void)c;

    alloc.Destroy(d);  // [12,4]
    alloc.Destroy(b);  // [4,4]

    auto first = alloc.Allocate(3);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->Start, 4);
    EXPECT_EQ(first->Length, 3);

    auto second = alloc.Allocate(1);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->Start, 7);
    EXPECT_EQ(second->Length, 1);
}

TEST(Core_Allocator_FirstFit, OverAllocate) {
    FirstFitAllocator alloc{4};
    auto a = alloc.Allocate(2);
    ASSERT_TRUE(a.has_value());
    EXPECT_FALSE(alloc.Allocate(3).has_value());

    alloc.Destroy(*a);
    auto full = alloc.Allocate(4);
    ASSERT_TRUE(full.has_value());
    EXPECT_EQ(full->Start, 0);
}

TEST(Core_Allocator_FirstFit, RandomStress) {
    constexpr size_t kCapacity = 4096;
    FirstFitAllocator alloc{kCapacity};
    std::vector<FirstFitAllocator::Allocation> active;
    std::mt19937 rng(42);

    for (int i = 0; i < 5000; ++i) {
        const bool doAlloc = active.empty() || std::uniform_int_distribution<int>(0, 2)(rng) != 0;
        if (doAlloc) {
            const size_t size = std::uniform_int_distribution<size_t>(1, 128)(rng);
            auto block = alloc.Allocate(size);
            if (!block.has_value()) {
                continue;
            }

            for (const auto& e : active) {
                const size_t eEnd = e.Start + e.Length;
                const size_t bEnd = block->Start + block->Length;
                EXPECT_TRUE(bEnd <= e.Start || eEnd <= block->Start);
            }
            active.emplace_back(*block);
            continue;
        }

        const size_t idx = std::uniform_int_distribution<size_t>(0, active.size() - 1)(rng);
        alloc.Destroy(active[idx]);
        active.erase(active.begin() + idx);
    }

    for (const auto& a : active) {
        alloc.Destroy(a);
    }

    auto all = alloc.Allocate(kCapacity);
    ASSERT_TRUE(all.has_value());
    EXPECT_EQ(all->Start, 0);
    EXPECT_EQ(all->Length, kCapacity);
}
