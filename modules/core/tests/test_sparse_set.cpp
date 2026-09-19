#include <gtest/gtest.h>

#include <memory>
#include <algorithm>

#include <radray/sparse_set.h>

using namespace radray;

namespace {

struct MoveOnlyValue {
    explicit MoveOnlyValue(int value) : Value(std::make_unique<int>(value)) {}

    MoveOnlyValue(MoveOnlyValue&&) noexcept = default;
    MoveOnlyValue& operator=(MoveOnlyValue&&) noexcept = default;

    MoveOnlyValue(const MoveOnlyValue&) = delete;
    MoveOnlyValue& operator=(const MoveOnlyValue&) = delete;

    std::unique_ptr<int> Value;
};

}  // namespace

TEST(SparseSetTest, EmplaceAndGet) {
    SparseSet<int> set;
    const SparseSetHandle handle = set.Emplace(42);

    ASSERT_TRUE(handle.IsValid());
    EXPECT_EQ(handle.Generation, 0u);
    EXPECT_TRUE(set.IsAlive(handle));
    ASSERT_NE(set.TryGet(handle), nullptr);
    EXPECT_EQ(*set.TryGet(handle), 42);
    EXPECT_EQ(set.Get(handle), 42);
    EXPECT_EQ(set.Count(), 1u);
    EXPECT_FALSE(set.Empty());
}

TEST(SparseSetTest, DestroyAndReuse) {
    SparseSet<int> set;
    const SparseSetHandle handleA = set.Emplace(10);
    const SparseSetHandle handleB = set.Emplace(20);

    set.Destroy(handleA);

    EXPECT_FALSE(set.IsAlive(handleA));
    EXPECT_EQ(set.TryGet(handleA), nullptr);

    const SparseSetHandle handleC = set.Emplace(30);
    EXPECT_EQ(handleC.Index, handleA.Index);
    EXPECT_NE(handleC.Generation, handleA.Generation);
    EXPECT_TRUE(set.IsAlive(handleB));
    EXPECT_TRUE(set.IsAlive(handleC));
    EXPECT_EQ(set.Get(handleB), 20);
    EXPECT_EQ(set.Get(handleC), 30);
}

TEST(SparseSetTest, SwapAndPop) {
    SparseSet<int> set;
    const SparseSetHandle handleA = set.Emplace(1);
    const SparseSetHandle handleB = set.Emplace(2);
    const SparseSetHandle handleC = set.Emplace(3);

    set.Destroy(handleB);

    EXPECT_TRUE(set.IsAlive(handleA));
    EXPECT_FALSE(set.IsAlive(handleB));
    EXPECT_TRUE(set.IsAlive(handleC));
    EXPECT_EQ(set.Get(handleA), 1);
    EXPECT_EQ(set.Get(handleC), 3);

    const auto values = set.Values();
    ASSERT_EQ(values.size(), 2u);
    EXPECT_EQ(values[0], 1);
    EXPECT_EQ(values[1], 3);
}

TEST(SparseSetTest, IterateDense) {
    SparseSet<int> set;
    const SparseSetHandle handleA = set.Emplace(4);
    const SparseSetHandle handleB = set.Emplace(5);
    const SparseSetHandle handleC = set.Emplace(6);

    set.Destroy(handleA);

    EXPECT_FALSE(set.IsAlive(handleA));
    EXPECT_TRUE(set.IsAlive(handleB));
    EXPECT_TRUE(set.IsAlive(handleC));

    const auto values = set.Values();
    ASSERT_EQ(values.size(), 2u);
    EXPECT_EQ(values[0], 6);
    EXPECT_EQ(values[1], 5);
}

TEST(SparseSetTest, ClearInvalidatesAllHandles) {
    SparseSet<int> set;
    const SparseSetHandle handleA = set.Emplace(7);
    const SparseSetHandle handleB = set.Emplace(8);

    set.Clear();

    EXPECT_TRUE(set.Empty());
    EXPECT_EQ(set.Count(), 0u);
    EXPECT_FALSE(set.IsAlive(handleA));
    EXPECT_FALSE(set.IsAlive(handleB));
    EXPECT_EQ(set.TryGet(handleA), nullptr);
    EXPECT_EQ(set.TryGet(handleB), nullptr);

    const SparseSetHandle handleC = set.Emplace(9);
    EXPECT_EQ(handleC.Index, handleA.Index);
    EXPECT_NE(handleC.Generation, handleA.Generation);
    EXPECT_TRUE(set.IsAlive(handleC));
    EXPECT_EQ(set.Get(handleC), 9);
}

TEST(SparseSetTest, ReserveSparseOnly) {
    SparseSet<int> set;

    set.Reserve(32);

    EXPECT_TRUE(set.Empty());
    EXPECT_EQ(set.Count(), 0u);

    const SparseSetHandle handle = set.Emplace(11);
    EXPECT_TRUE(handle.IsValid());
    EXPECT_TRUE(set.IsAlive(handle));
    EXPECT_EQ(set.Get(handle), 11);
}

TEST(SparseSetTest, MoveOnlyElement) {
    SparseSet<MoveOnlyValue> set;
    const SparseSetHandle handleA = set.Emplace(12);
    const SparseSetHandle handleB = set.Emplace(34);

    EXPECT_EQ(*set.Get(handleA).Value, 12);
    EXPECT_EQ(*set.Get(handleB).Value, 34);

    set.Destroy(handleA);

    EXPECT_FALSE(set.IsAlive(handleA));
    EXPECT_TRUE(set.IsAlive(handleB));
    EXPECT_EQ(*set.Get(handleB).Value, 34);
}

TEST(SparseSetTest, MixedOperationsPreserveAllHandlesAndDenseValues) {
    struct Entry {
        SparseSetHandle Handle;
        int Value;
        bool Alive;
    };
    SparseSet<int> set;
    vector<Entry> entries;
    vector<size_t> live;
    auto verify = [&]() {
        vector<int> expected;
        vector<uint32_t> indices;
        for (const auto& entry : entries) {
            ASSERT_EQ(set.IsAlive(entry.Handle), entry.Alive);
            const auto* value = std::as_const(set).TryGet(entry.Handle);
            if (entry.Alive) {
                ASSERT_NE(value, nullptr);
                EXPECT_EQ(*value, entry.Value);
                EXPECT_EQ(std::as_const(set).Get(entry.Handle), entry.Value);
                expected.push_back(entry.Value);
                indices.push_back(entry.Handle.Index);
            } else {
                EXPECT_EQ(value, nullptr);
            }
        }
        std::sort(indices.begin(), indices.end());
        EXPECT_EQ(std::adjacent_find(indices.begin(), indices.end()), indices.end());
        vector<int> actual(set.Values().begin(), set.Values().end());
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        EXPECT_EQ(actual, expected);
        EXPECT_EQ(set.Count(), live.size());
        EXPECT_EQ(set.Empty(), live.empty());
        EXPECT_FALSE(set.IsAlive(SparseSetHandle::Invalid()));
    };
    auto fill = [&]() {
        while (live.size() < 96) {
            const auto value = static_cast<int>(entries.size());
            const auto handle = set.Emplace(value);
            // Reuse every freed slot before growing beyond the live high-water mark.
            EXPECT_LT(handle.Index, 96u);
            live.push_back(entries.size());
            entries.push_back({handle, value, true});
            verify();
        }
    };
    for (size_t round = 0; round < 8; ++round) {
        fill();
        for (size_t i = 0; i < 48; ++i) {
            const size_t index = (i * 37 + round) % live.size();
            auto& entry = entries[live[index]];
            set.Destroy(entry.Handle);
            entry.Alive = false;
            live.erase(live.begin() + index);
            verify();
        }
        if (round % 2 == 0) fill();
        set.Clear();
        for (const auto index : live) entries[index].Alive = false;
        live.clear();
        verify();
        set.Clear();
        verify();
    }
}

#ifdef RADRAY_IS_DEBUG
TEST(SparseSetTest, InvalidGetDeathTest) {
    EXPECT_DEATH_IF_SUPPORTED(
        {
            SparseSet<int> set;
            const SparseSetHandle handle = set.Emplace(123);
            set.Destroy(handle);
            (void)set.Get(handle);
        },
        "");
}

TEST(SparseSetTest, DoubleDestroyDeathTest) {
    SparseSet<int> set;
    const auto handle = set.Emplace(1);
    set.Destroy(handle);
    EXPECT_DEATH_IF_SUPPORTED(set.Destroy(handle), "");
}
#endif
