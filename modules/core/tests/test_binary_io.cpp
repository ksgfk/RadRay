#include <radray/binary_io.h>

#include <algorithm>

#include <gtest/gtest.h>

namespace radray {
namespace {

TEST(BinaryIoTest, WriterEncodesLittleEndian) {
    BinaryWriter writer;

    writer.U8(0x7f);
    writer.U32(0x78563412u);
    writer.U64(0x8877665544332211ull);
    writer.I32(-2);
    writer.Float(1.0f);
    writer.Bool(true);

    const array<uint8_t, 22> expected{
        0x7f,
        0x12, 0x34, 0x56, 0x78,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0xfe, 0xff, 0xff, 0xff,
        0x00, 0x00, 0x80, 0x3f,
        0x01};
    EXPECT_TRUE(std::ranges::equal(writer.GetData(), std::as_bytes(std::span{expected})));
}

TEST(BinaryIoTest, ReaderDecodesWriterOutput) {
    BinaryWriter writer;
    writer.U8(200);
    writer.U32(0xdeadbeefu);
    writer.U64(0x0123456789abcdefull);
    writer.I32(-123456);
    writer.Float(-2.5f);
    writer.Bool(false);
    BinaryReader reader{writer.GetData()};

    uint8_t u8 = 0;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    int32_t i32 = 0;
    float f = 0;
    bool b = true;
    ASSERT_TRUE(reader.U8(u8) && reader.U32(u32) && reader.U64(u64) && reader.I32(i32) && reader.Float(f) && reader.Bool(b));

    EXPECT_EQ(u8, 200);
    EXPECT_EQ(u32, 0xdeadbeefu);
    EXPECT_EQ(u64, 0x0123456789abcdefull);
    EXPECT_EQ(i32, -123456);
    EXPECT_EQ(f, -2.5f);
    EXPECT_FALSE(b);
    EXPECT_TRUE(reader.AtEnd());
}

TEST(BinaryIoTest, StringsAndSizedBytesRoundTrip) {
    const array<byte, 3> payload{byte{0x10}, byte{0x20}, byte{0x30}};
    BinaryWriter writer;
    writer.String("RadRay");
    writer.String("");
    writer.SizedBytes(payload);
    BinaryReader reader{writer.GetData()};

    std::string_view text;
    std::string_view empty{"unchanged"};
    std::span<const byte> bytes;
    ASSERT_TRUE(reader.String(text));
    ASSERT_TRUE(reader.String(empty));
    ASSERT_TRUE(reader.SizedBytes(bytes));

    EXPECT_EQ(text, "RadRay");
    EXPECT_TRUE(empty.empty());
    EXPECT_TRUE(std::ranges::equal(bytes, payload));
    EXPECT_TRUE(reader.AtEnd());
}

TEST(BinaryIoTest, TakeDataMovesEncodedBytes) {
    BinaryWriter writer;
    writer.U32(1);

    const vector<byte> data = std::move(writer).TakeData();

    EXPECT_EQ(data.size(), 4u);
}

TEST(BinaryIoTest, TruncatedValueFailsWithoutConsumingInput) {
    const array<byte, 3> truncated{byte{1}, byte{2}, byte{3}};
    BinaryReader reader{truncated};
    uint32_t value = 42;

    EXPECT_FALSE(reader.U32(value));

    EXPECT_EQ(value, 42u);
    EXPECT_EQ(reader.Remaining(), truncated.size());
}

TEST(BinaryIoTest, BoolOtherThanZeroOrOneIsRejected) {
    const array<byte, 1> invalid{byte{2}};
    BinaryReader reader{invalid};
    bool value = true;

    EXPECT_FALSE(reader.Bool(value));

    EXPECT_TRUE(value);
    EXPECT_EQ(reader.Remaining(), 1u);
}

TEST(BinaryIoTest, LengthPrefixBeyondInputFailsWithoutConsumingInput) {
    BinaryWriter writer;
    writer.U32(5);
    writer.U8('x');
    BinaryWriter sized;
    sized.U64(5);
    sized.U8('x');
    BinaryReader stringReader{writer.GetData()};
    BinaryReader bytesReader{sized.GetData()};
    std::string_view text{"unchanged"};
    std::span<const byte> bytes;

    EXPECT_FALSE(stringReader.String(text));
    EXPECT_FALSE(bytesReader.SizedBytes(bytes));

    EXPECT_EQ(text, "unchanged");
    EXPECT_EQ(stringReader.Remaining(), writer.GetSize());
    EXPECT_EQ(bytesReader.Remaining(), sized.GetSize());
}

}  // namespace
}  // namespace radray
