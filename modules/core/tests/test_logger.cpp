#include <radray/logger.h>

#include <gtest/gtest.h>

namespace radray {
namespace {

TEST(FormatTest, PreservesLongStringsAndEmbeddedNulls) {
    string text(2048, 'x');
    text[1024] = '\0';

    const string result = radray::format("[{}]", text);

    EXPECT_EQ(result, "[" + text + "]");
    EXPECT_EQ(result.c_str()[result.size()], '\0');
}

TEST(FormatTest, AcceptsRuntimeFormatStrings) {
    const string pattern = "{1:0{0}x}";

    EXPECT_EQ(radray::format(fmt::runtime(pattern), 8, 42), "0000002a");
}

TEST(FormatTest, FormatsWideStringsWithEmbeddedNulls) {
    wstring text(2048, L'x');
    text[1024] = L'\0';

    const wstring result = radray::format(L"[{}]", text);

    EXPECT_EQ(result, L"[" + text + L"]");
    EXPECT_EQ(result.c_str()[result.size()], L'\0');
}

}  // namespace
}  // namespace radray
