#include <gtest/gtest.h>

#include <radray/utility.h>

TEST(Core_Utility, MBToWideChar) {
    radray::string a{"cajsijsaoi  aiosf jais fja pfjap s"};
    auto opt = radray::ToWideChar(a);
    radray::wstring wa = opt.value();
    radray::wstring ta{L"cajsijsaoi  aiosf jais fja pfjap s"};
    EXPECT_EQ(wa, ta);
}

TEST(Core_Utility, WideCharToMB) {
    radray::wstring a{L"abcdef"};
    auto opt = radray::ToMultiByte(a);
    radray::string wa = opt.value();
    radray::string ta{"abcdef"};
    EXPECT_EQ(wa, ta);
}
