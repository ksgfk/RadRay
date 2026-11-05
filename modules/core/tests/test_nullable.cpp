#include <radray/nullable.h>

#include <gtest/gtest.h>

#include <radray/types.h>

namespace radray {
namespace {

struct Base {
    virtual ~Base() noexcept = default;
    virtual int Get() const noexcept = 0;
};

struct Derived final : Base {
    explicit Derived(int value) noexcept : Value(value) {}
    int Get() const noexcept override { return Value; }
    int Value;
};

TEST(NullableTest, NullHasNoValue) {
    const Nullable<int*> value{nullptr};

    EXPECT_FALSE(value.HasValue());
    EXPECT_FALSE(value);
    EXPECT_TRUE(value == nullptr);
}

TEST(NullableTest, RawPointerGivesAccessWithoutReleasing) {
    int target = 42;
    Nullable<int*> value{&target};

    EXPECT_EQ(value.Get(), &target);
    EXPECT_EQ(*value, 42);
    EXPECT_EQ(value.Unwrap(), &target);
    EXPECT_TRUE(value.HasValue());
}

TEST(NullableTest, ReleaseEmptiesRawPointer) {
    int target = 42;
    Nullable<int*> value{&target};

    int* released = value.Release();

    EXPECT_EQ(released, &target);
    EXPECT_FALSE(value.HasValue());
}

TEST(NullableTest, ComparesUnderlyingPointers) {
    int a = 1;
    int b = 2;

    EXPECT_TRUE((Nullable<int*>{&a} == Nullable<int*>{&a}));
    EXPECT_TRUE((Nullable<int*>{&a} != Nullable<int*>{&b}));
    EXPECT_TRUE((Nullable<int*>{&a} != nullptr));
}

TEST(NullableTest, UniquePtrReleaseTransfersOwnership) {
    Nullable<unique_ptr<int>> value{make_unique<int>(7)};

    const unique_ptr<int> owner = value.Release();

    ASSERT_NE(owner, nullptr);
    EXPECT_EQ(*owner, 7);
    EXPECT_FALSE(value.HasValue());
}

TEST(NullableTest, UniquePtrUnwrapMovesOwnershipOut) {
    Nullable<unique_ptr<Base>> value{make_unique<Derived>(3)};
    EXPECT_EQ(value->Get(), 3);

    const unique_ptr<Base> owner = value.Unwrap();

    EXPECT_EQ(owner->Get(), 3);
    EXPECT_FALSE(value.HasValue());
}

TEST(NullableTest, UniquePtrMoveLeavesSourceEmpty) {
    Nullable<unique_ptr<int>> source{make_unique<int>(10)};

    const Nullable<unique_ptr<int>> target{std::move(source)};

    EXPECT_FALSE(source.HasValue());  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(*target.Get(), 10);
}

TEST(NullableTest, SharedPtrCopySharesOwnership) {
    const auto shared = make_shared<int>(20);
    const Nullable<shared_ptr<int>> first{shared};

    const Nullable<shared_ptr<int>> second = first;

    EXPECT_EQ(shared.use_count(), 3);
    EXPECT_EQ(second.Get(), shared.get());
}

TEST(NullableTest, SharedPtrConvertsFromDerived) {
    const Nullable<shared_ptr<Base>> value{make_shared<Derived>(5)};

    EXPECT_EQ(value->Get(), 5);
}

}  // namespace
}  // namespace radray
