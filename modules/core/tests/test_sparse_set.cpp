#include <gtest/gtest.h>

#include <memory>

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
