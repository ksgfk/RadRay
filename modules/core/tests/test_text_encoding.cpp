#include <gtest/gtest.h>

#include <radray/text_encoding.h>

using namespace radray;

TEST(TextEncodingTest, AsciiConvertsBothWays) {
    EXPECT_EQ(ToWideChar("abc 123"), wstring{L"abc 123"});
    EXPECT_EQ(ToMultiByte(L"abc 123"), string{"abc 123"});
}

TEST(TextEncodingTest, Utf8NonAsciiConvertsBothWays) {
    // "中文é" in UTF-8 and UTF-16.
    const string utf8{"\xE4\xB8\xAD\xE6\x96\x87\xC3\xA9"};
    const wstring wide{L"\u4E2D\u6587\u00E9"};

    EXPECT_EQ(ToWideChar(utf8), wide);
    EXPECT_EQ(ToMultiByte(wide), utf8);
}

TEST(TextEncodingTest, RoundTripPreservesEmbeddedNull) {
    const string text{"a\0b", 3};

    const auto wide = ToWideChar(text);

    ASSERT_TRUE(wide.has_value());
    EXPECT_EQ(wide->size(), 3u);
    EXPECT_EQ(ToMultiByte(*wide), text);
}
