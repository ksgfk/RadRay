#include <gtest/gtest.h>

#include <algorithm>
#include <array>

#include <radray/binary_io.h>

namespace radray {
namespace {

TEST(BinaryIoTest, WritesCanonicalLittleEndianValues) {
    BinaryWriter writer{32};
    writer.U8(0x7f);
    writer.U32(0x78563412u);
    writer.U64(0x8877665544332211ull);
    writer.I32(-2);
    writer.Float(1.0f);
    writer.Bool(true);
    writer.Bool(false);

    const std::array expected{
        byte{0x7f},
        byte{0x12},
        byte{0x34},
        byte{0x56},
        byte{0x78},
        byte{0x11},
        byte{0x22},
        byte{0x33},
        byte{0x44},
        byte{0x55},
        byte{0x66},
        byte{0x77},
        byte{0x88},
        byte{0xfe},
        byte{0xff},
        byte{0xff},
        byte{0xff},
        byte{0x00},
        byte{0x00},
        byte{0x80},
        byte{0x3f},
        byte{0x01},
        byte{0x00},
    };
    EXPECT_TRUE(std::ranges::equal(writer.GetData(), expected));
}

TEST(BinaryIoTest, RoundTripsStringsAndSizedBytes) {
    const std::array payload{byte{0x10}, byte{0x20}, byte{0x30}};
    BinaryWriter writer;
    writer.String("RadRay");
    writer.SizedBytes(payload);

    BinaryReader reader{writer.GetData()};
    std::string_view text;
    std::span<const byte> bytes;
    ASSERT_TRUE(reader.String(text));
    ASSERT_TRUE(reader.SizedBytes(bytes));
    EXPECT_EQ(text, "RadRay");
    EXPECT_TRUE(std::ranges::equal(bytes, payload));
    EXPECT_TRUE(reader.AtEnd());

    vector<byte> owned = std::move(writer).TakeData();
    EXPECT_FALSE(owned.empty());
}

TEST(BinaryIoTest, RejectsInvalidInputWithoutConsumingIt) {
    const std::array truncated{byte{0x01}, byte{0x02}, byte{0x03}};
    BinaryReader integerReader{truncated};
    uint32_t integer = 42;
    EXPECT_FALSE(integerReader.U32(integer));
    EXPECT_EQ(integer, 42u);
    EXPECT_EQ(integerReader.Remaining(), truncated.size());

    const std::array invalidBool{byte{0x02}};
    BinaryReader boolReader{invalidBool};
    bool flag = true;
    EXPECT_FALSE(boolReader.Bool(flag));
    EXPECT_TRUE(flag);
    EXPECT_EQ(boolReader.Remaining(), invalidBool.size());

    const std::array invalidString{
        byte{0x05},
        byte{0x00},
        byte{0x00},
        byte{0x00},
        byte{'x'},
    };
    BinaryReader stringReader{invalidString};
    std::string_view text{"unchanged"};
    EXPECT_FALSE(stringReader.String(text));
    EXPECT_EQ(text, "unchanged");
    EXPECT_EQ(stringReader.Remaining(), invalidString.size());
}

}  // namespace
}  // namespace radray
