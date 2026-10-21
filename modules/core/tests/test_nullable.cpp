#include <gtest/gtest.h>
#include <radray/nullable.h>

TEST(NullableTest, Null) {
    radray::Nullable<void*> n{nullptr};
    EXPECT_FALSE(n.HasValue());
    EXPECT_FALSE(n);
    EXPECT_TRUE(n == nullptr);
    EXPECT_FALSE(n != nullptr);
    EXPECT_THROW(n.Unwrap(), radray::NullableAccessException);
}

TEST(NullableTest, NotNull) {
    struct S {
        int x;
    };
    S s{42};
    radray::Nullable<S*> n{&s};
    EXPECT_TRUE(n.HasValue());
    EXPECT_TRUE(n);
    EXPECT_FALSE(n == nullptr);
    EXPECT_TRUE(n != nullptr);
    EXPECT_EQ(n->x, 42);

    EXPECT_EQ(n.Unwrap()->x, 42);
    EXPECT_TRUE(n.HasValue());

    auto ptr = n.Release();
    EXPECT_EQ(ptr->x, 42);
    EXPECT_FALSE(n.HasValue());
    EXPECT_FALSE(bool(n));
    EXPECT_TRUE(n == nullptr);
    EXPECT_FALSE(n != nullptr);
    EXPECT_THROW(n.Unwrap(), radray::NullableAccessException);
}

TEST(NullableTest, UniquePtr) {
    struct V {
        int x;
    };
    V s{42};
    radray::Nullable<std::unique_ptr<V>> n{std::make_unique<V>(s)};
    EXPECT_TRUE(n.HasValue());
    EXPECT_TRUE(n);
    EXPECT_FALSE(n == nullptr);
    EXPECT_TRUE(n != nullptr);
    EXPECT_EQ(n->x, 42);
    auto ptr = n.Release();
    EXPECT_EQ(ptr->x, 42);
    EXPECT_FALSE(n.HasValue());
    EXPECT_FALSE(bool(n));
    EXPECT_TRUE(n == nullptr);
    EXPECT_FALSE(n != nullptr);
    EXPECT_THROW(n.Unwrap(), radray::NullableAccessException);

    struct Base {
        virtual ~Base() = default;
        virtual int GetX() const = 0;
    };
    struct Derived final : Base {
        int x;
        Derived(int x) : x(x) {}
        ~Derived() override = default;
        int GetX() const override { return x; }
    };
    radray::Nullable<std::unique_ptr<Base>> n2{std::make_unique<Derived>(42)};
    EXPECT_EQ(n2->GetX(), 42);
    auto ptr2 = n2.Unwrap();
    EXPECT_EQ(ptr2->GetX(), 42);
    EXPECT_FALSE(n2.HasValue());
}

TEST(NullableTest, SharedPtr) {
    struct V {
        int x;
    };
    V s{42};
    radray::Nullable<std::shared_ptr<V>> n{std::make_shared<V>(s)};
    EXPECT_EQ(n._value.use_count(), 1);
    EXPECT_TRUE(n.HasValue());
    EXPECT_TRUE(n);
    EXPECT_FALSE(n == nullptr);
    EXPECT_TRUE(n != nullptr);
    EXPECT_EQ(n->x, 42);

    auto ptr = n.Release();
    EXPECT_EQ(ptr.use_count(), 1);
    EXPECT_EQ(ptr->x, 42);
    EXPECT_FALSE(n.HasValue());
    EXPECT_FALSE(bool(n));
    EXPECT_TRUE(n == nullptr);
    EXPECT_FALSE(n != nullptr);
    EXPECT_THROW(n.Unwrap(), radray::NullableAccessException);

    struct Base {
        virtual ~Base() = default;
        virtual int GetX() const = 0;
    };
    struct Derived final : Base {
        int x;
        Derived(int x) : x(x) {}
        ~Derived() override = default;
        int GetX() const override { return x; }
    };
    radray::Nullable<std::shared_ptr<Base>> n2{std::make_shared<Derived>(42)};
    EXPECT_EQ(n2->GetX(), 42);
}

TEST(NullableTest, UniquePtrMoveSemantics) {
    auto uptr = std::make_unique<int>(10);
    radray::Nullable<std::unique_ptr<int>> n1{std::move(uptr)};

    radray::Nullable<std::unique_ptr<int>> n2{std::move(n1)};
    EXPECT_FALSE(n1.HasValue());
    EXPECT_TRUE(n2.HasValue());
    EXPECT_EQ(*n2.Get(), 10);
}

TEST(NullableTest, SharedPtrCopySemantics) {
    auto sptr = std::make_shared<int>(20);
    radray::Nullable<std::shared_ptr<int>> n1{sptr};
    EXPECT_EQ(sptr.use_count(), 2);

    radray::Nullable<std::shared_ptr<int>> n2 = n1;
    EXPECT_EQ(sptr.use_count(), 3);
    EXPECT_TRUE(n1.HasValue());
    EXPECT_TRUE(n2.HasValue());
}

TEST(NullableTest, EqualityComparison) {
    int value = 42;
    radray::Nullable<int*> n1{&value};
    radray::Nullable<int*> n2{&value};
    radray::Nullable<int*> n3{nullptr};

    EXPECT_TRUE(n1 == n2);
    EXPECT_FALSE(n1 != n2);

    EXPECT_FALSE(n1 == n3);
    EXPECT_TRUE(n1 != n3);
}
