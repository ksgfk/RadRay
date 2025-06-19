#include <gtest/gtest.h>

#include <radray/utility.h>

using namespace radray;

TEST(Core_Utility, MBToWideChar) {
    string a{"cajsijsaoi  aiosf jais fja pfjap s"};
    auto opt = radray::ToWideChar(a);
    wstring wa = opt.value();
    wstring ta{L"cajsijsaoi  aiosf jais fja pfjap s"};
    EXPECT_EQ(wa, ta);
}

TEST(Core_Utility, WideCharToMB) {
    wstring a{L"abcdef"};
    auto opt = radray::ToMultiByte(a);
    string wa = opt.value();
    string ta{"abcdef"};
    EXPECT_EQ(wa, ta);
}
