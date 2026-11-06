#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <utility>

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

TEST(SparseSetTest, EmplacedValueIsReachableByHandle) {
    SparseSet<int> set;

    const SparseSetHandle handle = set.Emplace(42);

    ASSERT_TRUE(set.IsAlive(handle));
    EXPECT_EQ(set.Get(handle), 42);
    ASSERT_NE(set.TryGet(handle), nullptr);
    EXPECT_EQ(*set.TryGet(handle), 42);
    EXPECT_EQ(set.Count(), 1u);
    EXPECT_FALSE(set.Empty());
}

TEST(SparseSetTest, InvalidHandleIsNeverAlive) {
    SparseSet<int> set;
    set.Emplace(1);

    EXPECT_FALSE(set.IsAlive(SparseSetHandle::Invalid()));
    EXPECT_EQ(set.TryGet(SparseSetHandle::Invalid()), nullptr);
    EXPECT_FALSE(set.IsAlive(SparseSetHandle{100, 0}));
}

TEST(SparseSetTest, DestroyedHandleStaysDeadAfterSlotReuse) {
    SparseSet<int> set;
    const SparseSetHandle first = set.Emplace(10);
    const SparseSetHandle kept = set.Emplace(20);
    set.Destroy(first);

    const SparseSetHandle reused = set.Emplace(30);

    EXPECT_EQ(reused.Index, first.Index);
    EXPECT_NE(reused.Generation, first.Generation);
    EXPECT_FALSE(set.IsAlive(first));
    EXPECT_EQ(set.TryGet(first), nullptr);
    EXPECT_EQ(set.Get(kept), 20);
    EXPECT_EQ(set.Get(reused), 30);
}

TEST(SparseSetTest, DestroyMovesLastValueIntoHole) {
    SparseSet<int> set;
    const SparseSetHandle a = set.Emplace(1);
    const SparseSetHandle b = set.Emplace(2);
    const SparseSetHandle c = set.Emplace(3);

    set.Destroy(a);

    const vector<int> values(set.Values().begin(), set.Values().end());
    EXPECT_EQ(values, (vector<int>{3, 2}));
    EXPECT_EQ(set.GetHandleAt(0), c);
    EXPECT_EQ(set.GetHandleAt(1), b);
    EXPECT_EQ(set.Get(c), 3);
}

TEST(SparseSetTest, GetHandleAtMatchesDenseValues) {
    SparseSet<int> set;
    const SparseSetHandle a = set.Emplace(1);
    set.Emplace(2);
    set.Emplace(3);
    set.Destroy(a);
    set.Emplace(4);

    const auto& view = std::as_const(set);
    for (uint32_t i = 0; i < view.Count(); ++i) {
        EXPECT_EQ(&view.Get(view.GetHandleAt(i)), &view.Values()[i]);
    }
}

TEST(SparseSetTest, ClearInvalidatesAllHandles) {
    SparseSet<int> set;
    const SparseSetHandle a = set.Emplace(7);
    const SparseSetHandle b = set.Emplace(8);

    set.Clear();

    EXPECT_TRUE(set.Empty());
    EXPECT_FALSE(set.IsAlive(a));
    EXPECT_FALSE(set.IsAlive(b));
    const SparseSetHandle c = set.Emplace(9);
    EXPECT_FALSE(set.IsAlive(a));
    EXPECT_EQ(set.Get(c), 9);
}

TEST(SparseSetTest, ReserveDoesNotCreateElements) {
    SparseSet<int> set{32};

    EXPECT_TRUE(set.Empty());
    const SparseSetHandle handle = set.Emplace(11);
    EXPECT_EQ(set.Get(handle), 11);
}

TEST(SparseSetTest, ReservedValuesRemainAtStableAddressesUntilCapacityIsFilled) {
    SparseSet<int> set;
    set.Reserve(128);
    const auto first = set.Emplace(42);
    const auto address = &set.Get(first);
    for (int index = 1; index < 128; ++index) set.Emplace(index);
    EXPECT_EQ(&set.Get(first), address);
    EXPECT_EQ(set.Get(first), 42);
}

TEST(SparseSetTest, StoresMoveOnlyValues) {
    SparseSet<MoveOnlyValue> set;
    const SparseSetHandle a = set.Emplace(12);
    const SparseSetHandle b = set.Emplace(34);

    set.Destroy(a);

    EXPECT_EQ(*set.Get(b).Value, 34);
}

TEST(SparseSetTest, GeometricReservePreservesValuesAndAmortizesSmallGrowth) {
    SparseSet<int> set;
    const auto first = set.Emplace(42);
    size_t relocations = 0;
    for (uint32_t required = 2; required <= 128; ++required) {
        const auto previous = &set.Get(first);
        set.ReserveGeometric(required);
        const auto reserved = &set.Get(first);
        relocations += previous != reserved;
        set.Emplace(static_cast<int>(required));
        EXPECT_EQ(&set.Get(first), reserved);
    }
    EXPECT_LT(relocations, 20u);
    EXPECT_EQ(set.Get(first), 42);
}

TEST(SparseSetTest, MixedOperationsKeepHandlesConsistent) {
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
        for (const auto& entry : entries) {
            ASSERT_EQ(set.IsAlive(entry.Handle), entry.Alive);
            if (entry.Alive) {
                EXPECT_EQ(set.Get(entry.Handle), entry.Value);
                expected.push_back(entry.Value);
            }
        }
        vector<int> actual(set.Values().begin(), set.Values().end());
        std::sort(actual.begin(), actual.end());
        std::sort(expected.begin(), expected.end());
        EXPECT_EQ(actual, expected);
    };
    auto fill = [&]() {
        while (live.size() < 32) {
            const int value = static_cast<int>(entries.size());
            const SparseSetHandle handle = set.Emplace(value);
            // Freed slots are reused before the sparse array grows.
            EXPECT_LT(handle.Index, 32u);
            live.push_back(entries.size());
            entries.push_back({handle, value, true});
        }
    };

    for (size_t round = 0; round < 4; ++round) {
        fill();
        for (size_t i = 0; i < 16; ++i) {
            const size_t index = (i * 7 + round) % live.size();
            set.Destroy(entries[live[index]].Handle);
            entries[live[index]].Alive = false;
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
        }
        verify();
        fill();
        verify();
        if (round % 2 == 1) {
            set.Clear();
            for (const size_t index : live) {
                entries[index].Alive = false;
            }
            live.clear();
            verify();
        }
    }
}
