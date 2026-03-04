#include <gtest/gtest.h>
#include <radray/enum_flags.h>

enum class TestFlags : uint32_t {
    None = 0,
    A = 1 << 0,
    B = 1 << 1,
    C = 1 << 2,
    D = 1 << 3,
};

namespace radray {
template <>
struct is_flags<TestFlags> : std::true_type {};
}  // namespace radray

TEST(EnumFlagsTest, BasicConstructionAndEvaluation) {
    radray::EnumFlags<TestFlags> flag1;
    EXPECT_EQ(flag1.value(), 0);
    EXPECT_FALSE(static_cast<bool>(flag1));

    radray::EnumFlags<TestFlags> flag2(TestFlags::A);
    EXPECT_EQ(flag2.value(), 1);
    EXPECT_TRUE(static_cast<bool>(flag2));
    EXPECT_TRUE(flag2.HasFlag(TestFlags::A));
    EXPECT_FALSE(flag2.HasFlag(TestFlags::B));
}

TEST(EnumFlagsTest, BitwiseOperators) {
    auto flagA = radray::EnumFlags<TestFlags>(TestFlags::A);
    auto flagB = radray::EnumFlags<TestFlags>(TestFlags::B);

    auto flagAB = flagA | flagB;
    EXPECT_TRUE(flagAB.HasFlag(TestFlags::A));
    EXPECT_TRUE(flagAB.HasFlag(TestFlags::B));
    EXPECT_FALSE(flagAB.HasFlag(TestFlags::C));
    EXPECT_EQ(flagAB.value(), 3);

    auto flagA_again = flagAB & flagA;
    EXPECT_EQ(flagA_again.value(), 1);

    auto flagB_only = flagAB ^ flagA;
    EXPECT_EQ(flagB_only.value(), 2);

    auto notA = ~flagA;
    EXPECT_FALSE(notA.HasFlag(TestFlags::A));
    EXPECT_TRUE(notA.HasFlag(TestFlags::B));
}

TEST(EnumFlagsTest, CompoundAssignmentOperators) {
    radray::EnumFlags<TestFlags> flag(TestFlags::A);
    flag |= TestFlags::B;
    EXPECT_TRUE(flag.HasFlag(TestFlags::A));
    EXPECT_TRUE(flag.HasFlag(TestFlags::B));

    flag &= TestFlags::A;
    EXPECT_TRUE(flag.HasFlag(TestFlags::A));
    EXPECT_FALSE(flag.HasFlag(TestFlags::B));

    flag ^= TestFlags::C;
    EXPECT_TRUE(flag.HasFlag(TestFlags::A));
    EXPECT_TRUE(flag.HasFlag(TestFlags::C));
}

TEST(EnumFlagsTest, EqualityAndInequality) {
    auto flag1 = radray::EnumFlags<TestFlags>(TestFlags::A) | TestFlags::B;
    auto flag2 = radray::EnumFlags<TestFlags>(TestFlags::B) | TestFlags::A;
    auto flag3 = radray::EnumFlags<TestFlags>(TestFlags::C);

    EXPECT_TRUE(flag1 == flag2);
    EXPECT_FALSE(flag1 == flag3);
    EXPECT_TRUE(flag1 != flag3);

    EXPECT_TRUE(flag1 == (TestFlags::A | TestFlags::B));
    EXPECT_TRUE((TestFlags::A | TestFlags::B) == flag1);
}
