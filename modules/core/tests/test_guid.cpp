#include <radray/guid.h>

#include <radray/logger.h>
#include <gtest/gtest.h>

namespace radray {
namespace {

constexpr Guid kSample{0x00112233, 0x4455, 0x6677, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

TEST(GuidTest, DefaultIsEmpty) {
    EXPECT_TRUE(Guid{}.IsEmpty());
    EXPECT_EQ(Guid{}, Guid::Empty());
    EXPECT_FALSE(kSample.IsEmpty());
}

TEST(GuidTest, ComponentConstructorStoresFieldsBigEndian) {
    const Guid::ByteArray expected{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

    EXPECT_EQ(kSample.Bytes(), expected);
    EXPECT_EQ(kSample.Version(), 6u);
}

TEST(GuidTest, ToStringSupportsStandardFormats) {
    EXPECT_EQ(kSample.ToString(), "00112233-4455-6677-8899-aabbccddeeff");
    EXPECT_EQ(kSample.ToString('N'), "00112233445566778899aabbccddeeff");
    EXPECT_EQ(kSample.ToString('b'), "{00112233-4455-6677-8899-aabbccddeeff}");
    EXPECT_EQ(kSample.ToString('P'), "(00112233-4455-6677-8899-aabbccddeeff)");
    EXPECT_EQ(radray::format("{}", kSample), kSample.ToString());
}

TEST(GuidTest, TryParseAcceptsStandardFormatsCaseAndSurroundingWhitespace) {
    for (std::string_view text : {
             "00112233-4455-6677-8899-AABBCCDDEEFF",
             "00112233445566778899aabbccddeeff",
             "{00112233-4455-6677-8899-aabbccddeeff}",
             "(00112233-4455-6677-8899-aabbccddeeff)",
             " \t00112233-4455-6677-8899-aabbccddeeff\n"}) {
        Guid parsed;
        EXPECT_TRUE(Guid::TryParse(text, parsed)) << text;
        EXPECT_EQ(parsed, kSample) << text;
    }
}

TEST(GuidTest, TryParseRejectsMalformedTextAndKeepsResult) {
    for (std::string_view text : {
             "",
             "00112233-4455-6677-8899-aabbccddeef",
             "00112233-4455-6677-8899-aabbccddeefg",
             "001122334-455-6677-8899-aabbccddeeff",
             "{00112233-4455-6677-8899-aabbccddeeff)"}) {
        Guid result = kSample;
        EXPECT_FALSE(Guid::TryParse(text, result)) << text;
        EXPECT_EQ(result, kSample) << text;
    }
}

TEST(GuidTest, NewGuidReturnsDistinctNonEmptyValues) {
    const Guid first = Guid::NewGuid();
    const Guid second = Guid::NewGuid();

    EXPECT_FALSE(first.IsEmpty());
    EXPECT_NE(first, second);
}

TEST(GuidTest, HashesEqualValuesEqually) {
    unordered_set<Guid> set{kSample, Guid{kSample.Bytes()}, Guid{}};

    EXPECT_EQ(set.size(), 2u);
    EXPECT_EQ(std::hash<Guid>{}(kSample), std::hash<Guid>{}(Guid{kSample.Bytes()}));
}

}  // namespace
}  // namespace radray
