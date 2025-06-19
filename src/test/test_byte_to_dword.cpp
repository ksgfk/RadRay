#include <gtest/gtest.h>

#include <radray/utility.h>

using namespace radray;

TEST(Core_Utility, ByteToDWORD) {
    // 00000111 00001010 00001101 00001000
    // 00000001 00000000 00000000 00000000
    radray::uint8_t a[] = {0b111, 0b1010, 0b1101, 0b1000, 0b1};
    vector<uint32_t> dwords = radray::ByteToDWORD(a);
    {
        uint32_t r = 0b00001000'00001101'00001010'00000111;
        ASSERT_EQ(dwords[0], r);
    }
    {
        uint32_t r = 0b00000001;
        ASSERT_EQ(dwords[1], r);
    }
}
