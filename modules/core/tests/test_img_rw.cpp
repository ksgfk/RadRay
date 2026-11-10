#include <radray/image_data.h>

#include <radray/logger.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include <radray/enum_flags.h>
#include <radray/nullable.h>
#include <radray/scope_guard.h>

#include "../src/png_codec.h"

namespace radray {
namespace {

class ScopedTempFile {
public:
    explicit ScopedTempFile(std::string_view name)
        : _path(std::filesystem::temp_directory_path() / radray::format("radray_core_png_{}", name)) {}
    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;
    ~ScopedTempFile() noexcept {
        std::error_code ec;
        std::filesystem::remove(_path, ec);
    }

    string PathString() const { return string{_path.string()}; }

private:
    std::filesystem::path _path;
};

ImageData MakeGradient(uint32_t width, uint32_t height, ImageFormat format) {
    ImageData image;
    image.Width = width;
    image.Height = height;
    image.Format = format;
    image.Data = make_unique<byte[]>(image.GetSize());
    for (size_t i = 0; i < image.GetSize(); ++i) image.Data[i] = static_cast<byte>((i * 37 + 11) & 0xff);
    return image;
}

std::optional<ImageData> WriteAndLoad(const ImageData& image, std::string_view name, PNGLoadSettings settings = {}) {
    const ScopedTempFile file{name};
    const string path = file.PathString();
    if (!image.WritePNG({path, false})) return std::nullopt;
    std::ifstream stream{path.c_str(), std::ios::binary};
    return ImageData::LoadPNG(stream, settings);
}

bool SamePixels(const ImageData& a, const ImageData& b) {
    return std::ranges::equal(a.GetSpan(), b.GetSpan());
}

string Rgb16Png() {
    constexpr char encoded[] =
        "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d"
        "\x49\x48\x44\x52\x00\x00\x00\x02\x00\x00\x00\x01"
        "\x10\x02\x00\x00\x00\x2b\xd0\x34\x9e\x00\x00\x00"
        "\x15\x49\x44\x41\x54\x78\x9c\x63\x10\x32\x59\x7d"
        "\xf6\x3f\x03\x03\x03\x23\xc3\xff\xff\x00\x1b\xfd"
        "\x04\xbd\xe2\x63\x4e\x5e\x00\x00\x00\x00\x49\x45"
        "\x4e\x44\xae\x42\x60\x82";
    return string{encoded, sizeof(encoded) - 1};
}

string PalettePng() {
    constexpr char encoded[] =
        "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d"
        "\x49\x48\x44\x52\x00\x00\x00\x02\x00\x00\x00\x01"
        "\x08\x03\x00\x00\x00\xc3\xfc\x8f\xb8\x00\x00\x00"
        "\x06\x50\x4c\x54\x45\xff\x00\x00\x00\xff\x00\xd2"
        "\x87\xef\x71\x00\x00\x00\x02\x74\x52\x4e\x53\x40"
        "\xff\xab\xe8\x6d\xb0\x00\x00\x00\x0b\x49\x44\x41"
        "\x54\x78\x9c\x63\x60\x60\x04\x00\x00\x04\x00\x02"
        "\xbf\x7a\x3f\x4a\x00\x00\x00\x00\x49\x45\x4e\x44"
        "\xae\x42\x60\x82";
    return string{encoded, sizeof(encoded) - 1};
}

string Gray4Png() {
    constexpr char encoded[] =
        "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d"
        "\x49\x48\x44\x52\x00\x00\x00\x02\x00\x00\x00\x01"
        "\x04\x00\x00\x00\x00\x14\xb9\xcd\x57\x00\x00\x00"
        "\x0a\x49\x44\x41\x54\x78\x9c\x63\xf8\x00\x00\x00"
        "\xf2\x00\xf1\x9c\xf1\x1d\xe6\x00\x00\x00\x00\x49"
        "\x45\x4e\x44\xae\x42\x60\x82";
    return string{encoded, sizeof(encoded) - 1};
}

string InterlacedPng() {
    constexpr char encoded[] =
        "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d"
        "\x49\x48\x44\x52\x00\x00\x00\x03\x00\x00\x00\x03"
        "\x08\x06\x00\x00\x01\x21\x2f\x85\x29\x00\x00\x00"
        "\x2f\x49\x44\x41\x54\x78\x9c\x05\xc1\xc1\x01\xc0"
        "\x20\x08\x04\xc1\x33\x4f\xf5\xa9\xd6\x03\x54\x06"
        "\x34\xbe\x99\x91\x2c\xd1\x17\x89\x8e\x35\x2f\x1a"
        "\x0d\x4f\x74\xbd\xd1\xb4\x62\x79\xb1\xa3\xf8\x01"
        "\x3c\x97\x10\x2d\x07\xf2\xf8\xf1\x00\x00\x00\x00"
        "\x49\x45\x4e\x44\xae\x42\x60\x82";
    return string{encoded, sizeof(encoded) - 1};
}

string InterlacedAllPassesPng() {
    constexpr char encoded[] =
        "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d"
        "\x49\x48\x44\x52\x00\x00\x00\x09\x00\x00\x00\x09"
        "\x08\x06\x00\x00\x01\x97\x96\x36\x86\x00\x00\x01"
        "\x05\x49\x44\x41\x54\x78\x9c\x05\xc1\x85\x6e\x50"
        "\x31\x18\x80\xd1\x1f\x1d\xc1\x47\x86\x13\x64\x48"
        "\x60\x1b\x12\x1c\x82\xd4\x5b\x34\x38\x3c\x4e\xe5"
        "\xb6\xb7\x6f\xfd\x71\x8e\x88\xca\x1c\x49\x19\xf9"
        "\xa6\x26\xff\xd2\x44\x0e\xba\x8c\xfc\x72\x13\xb9"
        "\xab\x16\xf6\xdc\xc2\xa3\xb4\x20\xfb\x4d\xe6\x70"
        "\xc8\xc8\x8e\x59\x78\x10\x16\xe4\x87\x99\xfc\x09"
        "\x13\xd9\x52\x95\x73\xa6\x72\xc1\x55\x2e\x85\xca"
        "\x95\x54\x91\x37\x6a\xf0\xce\x0c\x94\x1b\x98\x30"
        "\x70\x69\x20\xfb\x74\xe6\x80\xcd\x1c\xf2\x99\x8d"
        "\x98\x91\xb3\xba\x72\xde\x56\x2e\xfa\xca\xe5\x58"
        "\x91\x7b\x7a\x61\xd7\x2e\xdc\xf7\x0b\x0f\xe3\x82"
        "\xbc\xd5\x83\xf7\x76\xa0\xfd\xc0\xc6\x81\x7c\xd7"
        "\x93\x9f\x76\xf2\xdb\x4f\xfe\xc6\x89\x1c\x55\x85"
        "\x63\xba\x70\xdc\x14\x4e\xd8\xc2\x49\x57\x38\xe5"
        "\x0b\xa7\x43\x61\x33\x16\xce\xa4\x82\x5c\x55\x8d"
        "\x6b\xba\x71\xdd\x34\x6e\xd8\xc6\xb6\x6b\xdc\xf4"
        "\x8d\x5b\xa1\x71\x3b\x36\xee\xa4\x86\x3c\x56\x9d"
        "\x27\xba\xf3\xd4\x74\x9e\xd9\xce\x73\xd7\x79\xe1"
        "\x3b\x2f\x43\xe7\x55\xec\xbc\x4e\x1d\x09\x6a\x25"
        "\xea\x95\x64\x56\x3e\xd8\x95\x8f\x6e\xe5\x93\x5f"
        "\xf9\x1c\x56\xbe\xc4\x95\xaf\x69\xe5\x3f\x99\x60"
        "\x9d\xe4\x1e\xf4\x25\xfc\x00\x00\x00\x00\x49\x45"
        "\x4e\x44\xae\x42\x60\x82";
    return string{encoded, sizeof(encoded) - 1};
}

class PngRoundTripTest : public testing::TestWithParam<ImageFormat> {};

class NonSeekablePngInput : public std::streambuf {
public:
    explicit NonSeekablePngInput(string& encoded) {
        setg(encoded.data(), encoded.data(), encoded.data() + encoded.size());
    }
};

TEST_P(PngRoundTripTest, WrittenImageLoadsBackUnchanged) {
    const ImageData image = MakeGradient(7, 5, GetParam());

    const std::optional<ImageData> loaded = WriteAndLoad(image, EnumName(GetParam()));

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->Width, image.Width);
    EXPECT_EQ(loaded->Height, image.Height);
    EXPECT_EQ(loaded->Format, image.Format);
    EXPECT_TRUE(SamePixels(*loaded, image));
}

INSTANTIATE_TEST_SUITE_P(
    PngTest,
    PngRoundTripTest,
    testing::Values(ImageFormat::RGB8_BYTE, ImageFormat::RGBA8_BYTE),
    [](const testing::TestParamInfo<ImageFormat>& info) { return std::string{EnumName(info.param)}; });

TEST(PngTest, LoadWithFlipYReversesRows) {
    ImageData image = MakeGradient(3, 4, ImageFormat::RGBA8_BYTE);

    const std::optional<ImageData> loaded = WriteAndLoad(image, "flip", PNGLoadSettings{.AddAlphaIfRGB = std::nullopt, .IsFlipY = true});

    ASSERT_TRUE(loaded.has_value());
    image.FlipY();
    EXPECT_TRUE(SamePixels(*loaded, image));
}

TEST(PngTest, AddAlphaIfRGBExpandsToRGBA) {
    const ImageData rgb = MakeGradient(3, 2, ImageFormat::RGB8_BYTE);

    const std::optional<ImageData> loaded = WriteAndLoad(rgb, "alpha", PNGLoadSettings{.AddAlphaIfRGB = 77});

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->Format, ImageFormat::RGBA8_BYTE);
    EXPECT_TRUE(SamePixels(*loaded, rgb.RGB8ToRGBA8(77)));
}

TEST(PngTest, IsPNGChecksSignatureWithoutConsumingStream) {
    const ScopedTempFile file{"signature"};
    ASSERT_TRUE(MakeGradient(1, 1, ImageFormat::RGBA8_BYTE).WritePNG({file.PathString(), false}));
    std::ifstream png{file.PathString().c_str(), std::ios::binary};
    std::istringstream text{"definitely not a png"};

    EXPECT_TRUE(ImageData::IsPNG(png));
    EXPECT_EQ(png.tellg(), std::streampos{0});
    EXPECT_FALSE(ImageData::IsPNG(text));
}

TEST(PngTest, LoadRejectsNonPngData) {
    std::istringstream text{"definitely not a png"};

    EXPECT_FALSE(ImageData::LoadPNG(text).has_value());
}

TEST(PngTest, LoadRejectsTruncatedHeader) {
    std::istringstream stream{Rgb16Png().substr(0, 20)};

    EXPECT_FALSE(ImageData::LoadPNG(stream).has_value());
}

TEST(PngTest, LoadRejectsTruncatedPixelData) {
    std::istringstream stream{Rgb16Png().substr(0, 41)};

    EXPECT_FALSE(ImageData::LoadPNG(stream).has_value());
}

TEST(PngTest, LoadRejectsMissingEndChunk) {
    string encoded = Rgb16Png();
    encoded.resize(encoded.size() - 12);
    std::istringstream stream{encoded};

    EXPECT_FALSE(ImageData::LoadPNG(stream).has_value());
}

TEST(PngTest, LoadRejectsCorruptPixelCrc) {
    string encoded = Rgb16Png();
    encoded[encoded.size() - 13] ^= 1;
    std::istringstream stream{encoded};

    EXPECT_FALSE(ImageData::LoadPNG(stream).has_value());
}

TEST(PngTest, LoadTruncatedDataWithStreamExceptionsEnabledReturnsEmpty) {
    std::istringstream stream{Rgb16Png().substr(0, 41)};
    stream.exceptions(std::ios::failbit | std::ios::badbit);

    EXPECT_FALSE(ImageData::LoadPNG(stream).has_value());
}

TEST(PngTest, LoadPreserves16BitSamples) {
    std::istringstream stream{Rgb16Png()};
    const array<uint16_t, 6> expected{0x1234, 0xabcd, 0xff00, 0, 0x100, 0xffff};

    const auto loaded = ImageData::LoadPNG(stream);

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->Format, ImageFormat::RGB16_USHORT);
    EXPECT_TRUE(std::ranges::equal(loaded->GetSpan(), std::as_bytes(std::span{expected})));
}

TEST(PngTest, LoadSupportsNonSeekableInput) {
    string encoded = Rgb16Png();
    NonSeekablePngInput source{encoded};
    std::istream stream{&source};

    const auto loaded = ImageData::LoadPNG(stream);

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->Width, 2u);
    EXPECT_EQ(loaded->Height, 1u);
    EXPECT_EQ(loaded->Format, ImageFormat::RGB16_USHORT);
}

TEST(PngTest, LoadStartsAtCurrentStreamPosition) {
    std::istringstream stream{string{"prefix"} + Rgb16Png()};
    stream.seekg(6);

    const auto loaded = ImageData::LoadPNG(stream);

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->Width, 2u);
    EXPECT_EQ(loaded->Height, 1u);
}

TEST(PngTest, AddAlphaToRgb16UsesFull16BitValue) {
    std::istringstream stream{Rgb16Png()};
    const array<uint16_t, 8> expected{0x1234, 0xabcd, 0xff00, 0x3456, 0, 0x100, 0xffff, 0x3456};

    const auto loaded = ImageData::LoadPNG(stream, PNGLoadSettings{.AddAlphaIfRGB = 0x3456});

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->Format, ImageFormat::RGBA16_USHORT);
    EXPECT_TRUE(std::ranges::equal(loaded->GetSpan(), std::as_bytes(std::span{expected})));
}

TEST(PngTest, LoadExpandsLowBitDepthGray) {
    std::istringstream stream{Gray4Png()};
    const array<byte, 2> expected{byte{255}, byte{0}};

    const auto loaded = ImageData::LoadPNG(stream);

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->Format, ImageFormat::R8_BYTE);
    EXPECT_TRUE(std::ranges::equal(loaded->GetSpan(), expected));
}

TEST(PngTest, LoadPreservesPaletteTransparencyWhenAddingAlpha) {
    std::istringstream stream{PalettePng()};
    const array<byte, 8> expected{byte{255}, byte{0}, byte{0}, byte{64}, byte{0}, byte{255}, byte{0}, byte{255}};

    const auto loaded = ImageData::LoadPNG(stream, PNGLoadSettings{.AddAlphaIfRGB = 77});

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->Format, ImageFormat::RGBA8_BYTE);
    EXPECT_TRUE(std::ranges::equal(loaded->GetSpan(), expected));
}

TEST(PngTest, LoadInterlacedImageWithFlipY) {
    std::istringstream stream{InterlacedPng()};
    array<byte, 36> expected{};
    for (size_t y = 0; y < 3; ++y) {
        for (size_t x = 0; x < 3; ++x) {
            const size_t offset = (y * 3 + x) * 4;
            expected[offset] = static_cast<byte>(x + 10 * (2 - y));
            expected[offset + 1] = static_cast<byte>(64 + x);
            expected[offset + 2] = static_cast<byte>(128 + (2 - y));
            expected[offset + 3] = byte{255};
        }
    }

    const auto loaded = ImageData::LoadPNG(stream, PNGLoadSettings{.IsFlipY = true});

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->Format, ImageFormat::RGBA8_BYTE);
    EXPECT_TRUE(std::ranges::equal(loaded->GetSpan(), expected));
}

TEST(PngTest, WriteWithFlipYReversesRows) {
    const ScopedTempFile file{"write_flip"};
    ImageData image = MakeGradient(3, 4, ImageFormat::RGBA8_BYTE);
    ASSERT_TRUE(image.WritePNG({file.PathString(), true}));
    std::ifstream stream{file.PathString().c_str(), std::ios::binary};

    const auto loaded = ImageData::LoadPNG(stream);

    ASSERT_TRUE(loaded.has_value());
    image.FlipY();
    EXPECT_TRUE(SamePixels(*loaded, image));
}

TEST(PngTest, LoadInterlacedImageAcrossAllPassesWithFlipY) {
    std::istringstream stream{InterlacedAllPassesPng()};
    array<byte, 9 * 9 * 4> expected{};
    for (size_t y = 0; y < 9; ++y) {
        for (size_t x = 0; x < 9; ++x) {
            const size_t offset = (y * 9 + x) * 4;
            expected[offset] = static_cast<byte>(x + 10 * (8 - y));
            expected[offset + 1] = static_cast<byte>(64 + x);
            expected[offset + 2] = static_cast<byte>(128 + (8 - y));
            expected[offset + 3] = byte{255};
        }
    }

    const auto loaded = ImageData::LoadPNG(stream, PNGLoadSettings{.IsFlipY = true});

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->Width, 9u);
    EXPECT_EQ(loaded->Height, 9u);
    EXPECT_EQ(loaded->Format, ImageFormat::RGBA8_BYTE);
    EXPECT_TRUE(std::ranges::equal(loaded->GetSpan(), expected));
}

TEST(PngTest, WriteRejectsDirectoryDestination) {
    const string path{std::filesystem::temp_directory_path().string()};

    EXPECT_FALSE(MakeGradient(1, 1, ImageFormat::RGBA8_BYTE).WritePNG({path, false}));
}

TEST(PngTest, WriteBoundaryReturnsFailureForReadOnlyFile) {
    const ScopedTempFile file{"read_only"};
    const ImageData image = MakeGradient(1, 1, ImageFormat::RGBA8_BYTE);
    ASSERT_TRUE(image.WritePNG({file.PathString(), false}));
    Nullable<std::FILE*> destination{std::fopen(file.PathString().c_str(), "rb")};
    ASSERT_TRUE(destination);
    auto closeFile = MakeScopeGuard([&]() { std::fclose(destination.Get()); });
    RadRayPngState state{};
    state.Width = image.Width;
    state.Height = image.Height;
    state.Channels = 4;
    auto cleanup = MakeScopeGuard([&]() { radray_png_destroy(&state); });

    const bool written = radray_png_write(&state, destination.Get(), image.Data.get(), false);

    EXPECT_FALSE(written);
    EXPECT_NE(state.Error[0], '\0');
}

TEST(PngTest, WriteRejectsUnsupportedFormat) {
    const ScopedTempFile file{"unsupported"};
    const ImageData image = MakeGradient(4, 4, ImageFormat::R16_USHORT);

    EXPECT_FALSE(image.WritePNG({file.PathString(), false}));
}

}  // namespace
}  // namespace radray
