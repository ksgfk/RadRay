#include <radray/enum_flags.h>

#include <radray/logger.h>

#include <gtest/gtest.h>

enum class TestFlags : uint32_t {
    None = 0,
    A = 1 << 0,
    B = 1 << 1,
    C = 1 << 2,
    Far = 1 << 10,
};

enum class CompoundFlags : uint32_t {
    None = 0,
    Base = 1 << 0,
    A = Base | (Base << 1),
    B = Base | (Base << 2),
};

std::string_view format_as(CompoundFlags value) noexcept {
    switch (value) {
        case CompoundFlags::Base: return "Base";
        case CompoundFlags::A: return "A";
        case CompoundFlags::B: return "B";
        default: return "UNKNOWN";
    }
}

namespace radray {

template <>
struct is_flags<TestFlags> : std::true_type {};
template <>
struct is_flags<CompoundFlags> : std::true_type {};
template <>
struct is_compound_enum_flags<CompoundFlags> : std::true_type {};

namespace {

using Flags = EnumFlags<TestFlags>;

TEST(EnumFlagsTest, DefaultHasNoBits) {
    const Flags flags;

    EXPECT_EQ(flags.value(), 0u);
    EXPECT_FALSE(static_cast<bool>(flags));
}

TEST(EnumFlagsTest, BitwiseOperatorsCombineFlags) {
    const Flags ab = TestFlags::A | TestFlags::B;

    EXPECT_EQ(ab.value(), 3u);
    EXPECT_EQ((ab & TestFlags::A), TestFlags::A);
    EXPECT_EQ((ab ^ TestFlags::A), TestFlags::B);
    EXPECT_FALSE((~Flags{TestFlags::A}).HasFlag(TestFlags::A));
    EXPECT_TRUE((~Flags{TestFlags::A}).HasFlag(TestFlags::C));
}

TEST(EnumFlagsTest, CompoundAssignmentUpdatesValue) {
    Flags flags = TestFlags::A;

    flags |= TestFlags::B;
    flags &= TestFlags::B;
    flags ^= TestFlags::C;

    EXPECT_EQ(flags, TestFlags::B | TestFlags::C);
}

TEST(EnumFlagsTest, HasFlagRequiresEveryRequestedBit) {
    const Flags flags = TestFlags::A;

    EXPECT_TRUE(flags.HasFlag(TestFlags::A));
    EXPECT_FALSE(flags.HasFlag(TestFlags::A | TestFlags::B));
}

TEST(EnumFlagsTest, FormatsSetFlagsByName) {
    EXPECT_EQ(radray::format("{}", Flags{}), "[]");
    EXPECT_EQ(radray::format("{}", Flags{TestFlags::A}), "[A]");
    EXPECT_EQ(radray::format("{}", TestFlags::A | TestFlags::B | TestFlags::Far), "[A | B | Far]");
}

TEST(EnumFlagsTest, FormatsUnnamedBitsAsBinary) {
    const Flags unnamed{static_cast<TestFlags>(1u << 5)};

    EXPECT_EQ(unnamed.FormatByName(), "0b00000000000000000000000000100000");
}

TEST(EnumFlagsTest, CompoundFlagsFormatThroughAdlFormatAs) {
    EXPECT_EQ(radray::format("{}", EnumFlags<CompoundFlags>{CompoundFlags::A}), "[A]");
    EXPECT_EQ(radray::format("{}", EnumFlags<CompoundFlags>{CompoundFlags::A | CompoundFlags::B}), "[UNKNOWN]");
    EXPECT_EQ(radray::format("{}", EnumFlags<CompoundFlags>{}), "[]");
}

TEST(EnumFlagsTest, FlagBitNameReachesBitsOutsideReflectionRange) {
    EXPECT_TRUE(EnumName(TestFlags::Far).empty());
    EXPECT_EQ(EnumNameOr(TestFlags::Far, "fallback"), "fallback");
    EXPECT_EQ(EnumFlagBitName(TestFlags::Far), "Far");
    EXPECT_TRUE(EnumFlagBitName(static_cast<TestFlags>(TestFlags::A | TestFlags::B)).empty());
}

TEST(EnumFlagsTest, FlagsCastParsesNamesAndRejectsUndefinedBits) {
    EXPECT_EQ(EnumFlagsCast<TestFlags>("A|Far"), TestFlags::A | TestFlags::Far);
    EXPECT_FALSE(EnumFlagsCast<TestFlags>("A|Missing").has_value());
    EXPECT_EQ(EnumFlagsCast<TestFlags>(3u), TestFlags::A | TestFlags::B);
    EXPECT_FALSE(EnumFlagsCast<TestFlags>(1u << 5).has_value());
}

}  // namespace
}  // namespace radray
