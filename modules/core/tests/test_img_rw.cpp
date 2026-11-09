#include <radray/image_data.h>

#include <radray/logger.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include <radray/enum_flags.h>

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

class PngRoundTripTest : public testing::TestWithParam<ImageFormat> {};

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

TEST(PngTest, WriteRejectsUnsupportedFormat) {
    const ScopedTempFile file{"unsupported"};
    const ImageData image = MakeGradient(4, 4, ImageFormat::R16_USHORT);

    EXPECT_FALSE(image.WritePNG({file.PathString(), false}));
}

}  // namespace
}  // namespace radray
