#include <radray/shader/shader_metadata_wire.h>

#include <algorithm>
#include <limits>
#include <gtest/gtest.h>

namespace radray::shader {
namespace {

template <typename T>
void CheckRecordBounds(uint32_t size, uint32_t packedOffset, uint32_t usedBits) {
    T record{};
    ASSERT_EQ(GetShaderMetadataRecordSize(record), size);
    vector<byte> encoded(size, byte{0xa5});
    ASSERT_TRUE(EncodeShaderMetadataRecord(record, encoded));
    T output{};
    ASSERT_TRUE(DecodeShaderMetadataRecord(encoded, output));
    vector<byte> before(size);
    ASSERT_TRUE(EncodeShaderMetadataRecord(output, before));
    for (uint32_t length = 0; length < size; ++length) {
        EXPECT_FALSE(DecodeShaderMetadataRecord(std::span<const byte>{encoded}.first(length), output));
        vector<byte> destination(length, byte{0xa5});
        EXPECT_FALSE(EncodeShaderMetadataRecord(record, destination));
        EXPECT_TRUE(std::all_of(destination.begin(), destination.end(), [](byte value) { return value == byte{0xa5}; }));
    }
    for (uint32_t bit = usedBits; bit < 32; ++bit) {
        auto reserved = encoded;
        reserved[packedOffset + bit / 8] |= static_cast<byte>(1u << (bit % 8));
        EXPECT_FALSE(DecodeShaderMetadataRecord(reserved, output)) << "reserved bit " << bit;
        vector<byte> after(size);
        ASSERT_TRUE(EncodeShaderMetadataRecord(output, after));
        EXPECT_EQ(before, after);
    }
    encoded.push_back(byte{});
    EXPECT_FALSE(DecodeShaderMetadataRecord(encoded, output));
}

TEST(ShaderMetadataWire, RejectsTruncatedRecordsAndReservedBitsWithoutChangingOutput) {
    CheckRecordBounds<ShaderArtifactEntryRecord>(20, 8, 8);
    CheckRecordBounds<ShaderArtifactBindingRecord>(36, 16, 9);
    CheckRecordBounds<ShaderArtifactTypeRecord>(44, 12, 16);
    CheckRecordBounds<ShaderArtifactRootConstantRecord>(32, 24, 4);
    CheckRecordBounds<ShaderArtifactVertexInputRecord>(20, 16, 5);
    CheckRecordBounds<ShaderArtifactSamplerRecord>(20, 0, 23);
}

TEST(ShaderMetadataWire, BindingEncodingPreservesFullWidthFieldsInLittleEndian) {
    ShaderArtifactBindingRecord record{
        .Name = {0x12345678, 0x90abcdef}, .Group = 0x10203040, .Binding = 0xfedcba98, .Type = 10, .Count = std::numeric_limits<uint32_t>::max(), .StageMask = 7, .Placement = 2, .SamplerIndex = 0x01020304, .TypeIndex = 0x05060708, .BufferByteSize = 0x0a0b0c0d};
    const array<uint8_t, 36> expected{
        0x78, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x90, 0x40, 0x30, 0x20, 0x10, 0x98, 0xba, 0xdc, 0xfe,
        0x7a, 0x01, 0, 0, 0xff, 0xff, 0xff, 0xff, 4, 3, 2, 1, 8, 7, 6, 5, 0x0d, 0x0c, 0x0b, 0x0a};
    array<byte, 36> encoded{};
    ASSERT_TRUE(EncodeShaderMetadataRecord(record, encoded));
    EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), expected.begin(), [](byte a, uint8_t b) { return std::to_integer<uint8_t>(a) == b; }));
    ShaderArtifactBindingRecord decoded;
    ASSERT_TRUE(DecodeShaderMetadataRecord(std::as_bytes(std::span{expected}), decoded));
    EXPECT_EQ(decoded.Count, std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(decoded.Binding, record.Binding);
    EXPECT_EQ(decoded.Name.Offset, record.Name.Offset);
    EXPECT_EQ(decoded.Type, 10u);
    EXPECT_EQ(decoded.StageMask, 7u);
    EXPECT_EQ(decoded.Placement, 2u);
    EXPECT_EQ(decoded.BufferByteSize, record.BufferByteSize);
}

TEST(ShaderMetadataWire, MatrixArrayEncodingKeepsExplicitSpansAndShape) {
    // Two column-major float3x3 matrices: accessed span 92, element span 44.
    const array<uint8_t, 44> bytes{
        0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0x5d, 0x36, 0, 0,
        2, 0, 0, 0, 16, 0, 0, 0, 92, 0, 0, 0, 48, 0, 0, 0,
        0xff, 0xff, 0xff, 0xff, 44, 0, 0, 0, 16, 0, 0, 0};
    ShaderArtifactTypeRecord record;
    ASSERT_TRUE(DecodeShaderMetadataRecord(std::as_bytes(std::span{bytes}), record));
    EXPECT_EQ(record.Kind, uint32_t(ShaderTypeKind::Array));
    EXPECT_EQ(record.ElementKind, uint32_t(ShaderTypeKind::Matrix));
    EXPECT_EQ(record.ScalarKind, uint32_t(ShaderScalarKind::Float));
    EXPECT_EQ(record.RowCount, 3u);
    EXPECT_EQ(record.ColumnCount, 3u);
    EXPECT_EQ(record.Flags, 0u);
    EXPECT_EQ(record.Size, 92u);
    EXPECT_EQ(record.ElementSize, 44u);
    EXPECT_EQ(record.Stride, 48u);
    EXPECT_EQ(record.MatrixStride, 16u);
    array<byte, 44> encoded{};
    ASSERT_TRUE(EncodeShaderMetadataRecord(record, encoded));
    EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), bytes.begin(), [](byte a, uint8_t b) { return std::to_integer<uint8_t>(a) == b; }));
}

TEST(ShaderMetadataWire, SamplerFloatBitsAreNotQuantized) {
    ShaderArtifactSamplerRecord record{.MagFilter = 1, .MinFilter = 1, .AddressModeU = 4, .MipLodBias = -0.125f, .AnisotropyEnable = 1, .MaxAnisotropy = 15.75f, .CompareEnable = 1, .CompareOp = 7, .MinLod = -0.0f, .MaxLod = 1000.0f, .BorderColor = 5, .ReductionMode = 2, .Flags = 1};
    array<byte, 20> encoded{};
    ASSERT_TRUE(EncodeShaderMetadataRecord(record, encoded));
    ShaderArtifactSamplerRecord output;
    ASSERT_TRUE(DecodeShaderMetadataRecord(encoded, output));
    EXPECT_EQ(output, record);
    EXPECT_EQ(encoded[15], byte{0x80});  // Preserve the IEEE sign of negative zero.
}

template <typename T, typename Mutate>
void CheckEncoderRange(Mutate mutate) {
    T record{};
    mutate(record);
    vector<byte> bytes(GetShaderMetadataRecordSize(record), byte{0xa5});
    EXPECT_FALSE(EncodeShaderMetadataRecord(record, bytes));
    EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](byte b) { return b == byte{0xa5}; }));
}

TEST(ShaderMetadataWire, EncodersRejectOverflowAndUnusedFlagsWithoutWriting) {
    CheckEncoderRange<ShaderArtifactEntryRecord>([](auto& r) { r.Flags = 1; });
    CheckEncoderRange<ShaderArtifactBindingRecord>([](auto& r) { r.Type = 16; });
    CheckEncoderRange<ShaderArtifactBindingRecord>([](auto& r) { r.StageMask = 8; });
    CheckEncoderRange<ShaderArtifactBindingRecord>([](auto& r) { r.Placement = 4; });
    CheckEncoderRange<ShaderArtifactBindingRecord>([](auto& r) { r.Flags = 1; });
    CheckEncoderRange<ShaderArtifactTypeRecord>([](auto& r) { r.Kind = 8; });
    CheckEncoderRange<ShaderArtifactTypeRecord>([](auto& r) { r.ElementKind = 8; });
    CheckEncoderRange<ShaderArtifactTypeRecord>([](auto& r) { r.ScalarKind = 8; });
    CheckEncoderRange<ShaderArtifactTypeRecord>([](auto& r) { r.RowCount = 8; });
    CheckEncoderRange<ShaderArtifactTypeRecord>([](auto& r) { r.ColumnCount = 8; });
    CheckEncoderRange<ShaderArtifactTypeRecord>([](auto& r) { r.Flags = 2; });
    CheckEncoderRange<ShaderArtifactRootConstantRecord>([](auto& r) { r.StageMask = 8; });
    CheckEncoderRange<ShaderArtifactRootConstantRecord>([](auto& r) { r.Flags = 2; });
    CheckEncoderRange<ShaderArtifactVertexInputRecord>([](auto& r) { r.ComponentType = 4; });
    CheckEncoderRange<ShaderArtifactVertexInputRecord>([](auto& r) { r.ComponentCount = 8; });
    CheckEncoderRange<ShaderArtifactVertexInputRecord>([](auto& r) { r.Flags = 1; });
    CheckEncoderRange<ShaderArtifactSamplerRecord>([](auto& r) { r.MagFilter = 2; });
    CheckEncoderRange<ShaderArtifactSamplerRecord>([](auto& r) { r.MinFilter = 2; });
    CheckEncoderRange<ShaderArtifactSamplerRecord>([](auto& r) { r.MipmapMode = 2; });
    CheckEncoderRange<ShaderArtifactSamplerRecord>([](auto& r) { r.AddressModeU = 8; });
    CheckEncoderRange<ShaderArtifactSamplerRecord>([](auto& r) { r.AddressModeV = 8; });
    CheckEncoderRange<ShaderArtifactSamplerRecord>([](auto& r) { r.AddressModeW = 8; });
    CheckEncoderRange<ShaderArtifactSamplerRecord>([](auto& r) { r.AnisotropyEnable = 2; });
    CheckEncoderRange<ShaderArtifactSamplerRecord>([](auto& r) { r.CompareEnable = 2; });
    CheckEncoderRange<ShaderArtifactSamplerRecord>([](auto& r) { r.CompareOp = 8; });
    CheckEncoderRange<ShaderArtifactSamplerRecord>([](auto& r) { r.BorderColor = 8; });
    CheckEncoderRange<ShaderArtifactSamplerRecord>([](auto& r) { r.ReductionMode = 4; });
    CheckEncoderRange<ShaderArtifactSamplerRecord>([](auto& r) { r.Flags = 2; });
}

}  // namespace
}  // namespace radray::shader
