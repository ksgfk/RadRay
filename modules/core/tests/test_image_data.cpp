#include <radray/image_data.h>

#include <algorithm>

#include <gtest/gtest.h>

namespace radray {
namespace {

ImageData MakeImage(uint32_t width, uint32_t height, ImageFormat format, std::initializer_list<uint8_t> bytes) {
    ImageData image;
    image.Width = width;
    image.Height = height;
    image.Format = format;
    image.Data = make_unique<byte[]>(image.GetSize());
    std::transform(bytes.begin(), bytes.end(), image.Data.get(), [](uint8_t value) { return byte{value}; });
    return image;
}

vector<uint8_t> Bytes(const ImageData& image) {
    vector<uint8_t> result;
    for (byte value : image.GetSpan()) result.push_back(std::to_integer<uint8_t>(value));
    return result;
}

TEST(ImageDataTest, SizeIsPixelCountTimesFormatSize) {
    const ImageData image = MakeImage(3, 2, ImageFormat::RGB16_USHORT, {});

    EXPECT_EQ(image.GetSize(), 3u * 2u * 6u);
    EXPECT_EQ(ImageData{}.GetSize(), 0u);
    EXPECT_TRUE(ImageData{}.GetSpan().empty());
}

TEST(ImageDataTest, CopyOwnsIndependentPixels) {
    const ImageData source = MakeImage(1, 1, ImageFormat::RG8_BYTE, {1, 2});

    ImageData copy = source;
    copy.Data[0] = byte{9};

    EXPECT_EQ(Bytes(source), (vector<uint8_t>{1, 2}));
    EXPECT_EQ(Bytes(copy), (vector<uint8_t>{9, 2}));
}

TEST(ImageDataTest, MoveLeavesSourceEmpty) {
    ImageData source = MakeImage(1, 1, ImageFormat::RG8_BYTE, {1, 2});

    const ImageData moved = std::move(source);

    EXPECT_EQ(Bytes(moved), (vector<uint8_t>{1, 2}));
    EXPECT_EQ(source.GetSize(), 0u);  // NOLINT(bugprone-use-after-move)
}

TEST(ImageDataTest, RGB8ToRGBA8AppendsAlpha) {
    const ImageData rgb = MakeImage(2, 1, ImageFormat::RGB8_BYTE, {1, 2, 3, 4, 5, 6});

    const ImageData rgba = rgb.RGB8ToRGBA8(200);

    EXPECT_EQ(rgba.Format, ImageFormat::RGBA8_BYTE);
    EXPECT_EQ(Bytes(rgba), (vector<uint8_t>{1, 2, 3, 200, 4, 5, 6, 200}));
}

TEST(ImageDataTest, FlipYReversesRows) {
    ImageData image = MakeImage(2, 3, ImageFormat::R8_BYTE, {1, 2, 3, 4, 5, 6});

    image.FlipY();

    EXPECT_EQ(Bytes(image), (vector<uint8_t>{5, 6, 3, 4, 1, 2}));
}

TEST(ImageDataTest, CompareRGBA8CountsPixelsBeyondTolerance) {
    const ImageData expected = MakeImage(3, 1, ImageFormat::RGBA8_BYTE, {10, 10, 10, 255, 10, 10, 10, 255, 10, 10, 10, 255});
    const ImageData actual = MakeImage(3, 1, ImageFormat::RGBA8_BYTE, {12, 10, 10, 255, 10, 20, 10, 255, 10, 10, 0, 255});

    const PixelCompareResult result = ImageData::CompareImageRGBA8(actual, expected, 2);

    EXPECT_FALSE(result.IsMatch());
    EXPECT_EQ(result.MismatchCount, 2u);
    EXPECT_EQ(result.FirstMismatchPixel, 1u);
    EXPECT_EQ(result.FirstMismatchChannel, 1u);
    EXPECT_EQ(result.ActualValue, 20u);
    EXPECT_EQ(result.ExpectedValue, 10u);
}

TEST(ImageDataTest, CompareRGBA8WithinToleranceMatches) {
    const ImageData expected = MakeImage(1, 1, ImageFormat::RGBA8_BYTE, {10, 10, 10, 255});
    const ImageData actual = MakeImage(1, 1, ImageFormat::RGBA8_BYTE, {11, 9, 10, 254});

    EXPECT_TRUE(ImageData::CompareImageRGBA8(actual, expected, 1).IsMatch());
}

TEST(ImageDataTest, CompareRGBA8ReportsIncomparableImages) {
    const ImageData rgba = MakeImage(1, 1, ImageFormat::RGBA8_BYTE, {0, 0, 0, 0});
    const ImageData wider = MakeImage(2, 1, ImageFormat::RGBA8_BYTE, {});
    const ImageData rgb = MakeImage(1, 1, ImageFormat::RGB8_BYTE, {0, 0, 0});

    EXPECT_FALSE(ImageData::CompareImageRGBA8(rgba, wider, 0).ErrorReason.empty());
    EXPECT_FALSE(ImageData::CompareImageRGBA8(rgb, rgb, 0).ErrorReason.empty());
}

TEST(ImageDataTest, DiffRGBA8StoresAbsoluteColorDifferenceWithOpaqueAlpha) {
    const ImageData a = MakeImage(1, 1, ImageFormat::RGBA8_BYTE, {10, 50, 0, 7});
    const ImageData b = MakeImage(1, 1, ImageFormat::RGBA8_BYTE, {30, 20, 0, 9});

    const ImageData diff = ImageData::ImageDiffRGBA8(a, b);

    EXPECT_EQ(Bytes(diff), (vector<uint8_t>{20, 30, 0, 255}));
}

}  // namespace
}  // namespace radray
