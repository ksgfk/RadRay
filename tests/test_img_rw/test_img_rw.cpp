#define _CRT_SECURE_NO_WARNINGS

#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <vector>
#include <memory>

#include <gtest/gtest.h>
#include <radray/image_data.h>

static std::filesystem::path GetAssetsPath() {
    if (auto env = std::getenv("RADRAY_ASSETS_DIR")) {
        return std::filesystem::path{env};
    }
    return "assets";
}

static std::filesystem::path MakeTempFilePath(const char* name) {
    auto base = std::filesystem::temp_directory_path() / "radray_test_img_rw";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base / name;
}

TEST(PNG, SimpleLoad) {
    auto filename = GetAssetsPath() / "1735141462310.png";
    std::ifstream file{filename, std::ios::binary};
    ASSERT_TRUE(file.is_open());
    auto result = radray::ImageData::LoadPNG(file);
    ASSERT_TRUE(result.has_value());
    radray::ImageData img = std::move(result.value());
    ASSERT_EQ(img.Width, 2560);
    ASSERT_EQ(img.Height, 1440);
}

TEST(PNG, ToRGBA8) {
    auto filename = GetAssetsPath() / "1735141462310.png";
    std::ifstream file{filename, std::ios::binary};
    ASSERT_TRUE(file.is_open());
    auto result = radray::ImageData::LoadPNG(file);
    ASSERT_TRUE(result.has_value());
    radray::ImageData img = std::move(result.value());
    ASSERT_EQ(img.Width, 2560);
    ASSERT_EQ(img.Height, 1440);

    radray::ImageData rgba = img.RGB8ToRGBA8(0xff);
    ASSERT_EQ(rgba.Width, 2560);
    ASSERT_EQ(rgba.Height, 1440);
    ASSERT_EQ(rgba.Format, radray::ImageFormat::RGBA8_BYTE);
}

TEST(PNG, WriteReadRoundTripRGBA8) {
    radray::ImageData img;
    img.Width = 8;
    img.Height = 4;
    img.Format = radray::ImageFormat::RGBA8_BYTE;
    img.Data = std::make_unique<radray::byte[]>(img.GetSize());

    for (uint32_t y = 0; y < img.Height; ++y) {
        for (uint32_t x = 0; x < img.Width; ++x) {
            const size_t p = static_cast<size_t>(y) * img.Width * 4 + static_cast<size_t>(x) * 4;
            img.Data[p + 0] = static_cast<radray::byte>((x * 17) & 0xFF);
            img.Data[p + 1] = static_cast<radray::byte>((y * 33) & 0xFF);
            img.Data[p + 2] = static_cast<radray::byte>(((x + y) * 23) & 0xFF);
            img.Data[p + 3] = static_cast<radray::byte>(255);
        }
    }

    auto path = MakeTempFilePath("roundtrip_rgba8.png");
    ASSERT_TRUE(img.WritePNG({path.string(), false}));

    std::ifstream file{path, std::ios::binary};
    ASSERT_TRUE(file.is_open());
    auto loaded = radray::ImageData::LoadPNG(file);
    ASSERT_TRUE(loaded.has_value());

    ASSERT_EQ(loaded->Width, img.Width);
    ASSERT_EQ(loaded->Height, img.Height);
    ASSERT_EQ(loaded->Format, radray::ImageFormat::RGBA8_BYTE);
    ASSERT_EQ(loaded->GetSize(), img.GetSize());
    EXPECT_EQ(std::memcmp(loaded->Data.get(), img.Data.get(), img.GetSize()), 0);
}

TEST(PNG, WriteReadRoundTripRGB8) {
    radray::ImageData img;
    img.Width = 7;
    img.Height = 5;
    img.Format = radray::ImageFormat::RGB8_BYTE;
    img.Data = std::make_unique<radray::byte[]>(img.GetSize());

    for (uint32_t y = 0; y < img.Height; ++y) {
        for (uint32_t x = 0; x < img.Width; ++x) {
            const size_t p = static_cast<size_t>(y) * img.Width * 3 + static_cast<size_t>(x) * 3;
            img.Data[p + 0] = static_cast<radray::byte>((x * 19 + y) & 0xFF);
            img.Data[p + 1] = static_cast<radray::byte>((y * 27 + x) & 0xFF);
            img.Data[p + 2] = static_cast<radray::byte>(((x * 3 + y * 5) * 11) & 0xFF);
        }
    }

    auto path = MakeTempFilePath("roundtrip_rgb8.png");
    ASSERT_TRUE(img.WritePNG({path.string(), false}));

    std::ifstream file{path, std::ios::binary};
    ASSERT_TRUE(file.is_open());
    auto loaded = radray::ImageData::LoadPNG(file);
    ASSERT_TRUE(loaded.has_value());

    ASSERT_EQ(loaded->Width, img.Width);
    ASSERT_EQ(loaded->Height, img.Height);
    ASSERT_EQ(loaded->Format, radray::ImageFormat::RGB8_BYTE);
    ASSERT_EQ(loaded->GetSize(), img.GetSize());
    EXPECT_EQ(std::memcmp(loaded->Data.get(), img.Data.get(), img.GetSize()), 0);
}

TEST(PNG, WriteUnsupportedFormatFails) {
    radray::ImageData img;
    img.Width = 4;
    img.Height = 4;
    img.Format = radray::ImageFormat::R16_USHORT;
    img.Data = std::make_unique<radray::byte[]>(img.GetSize());
    auto path = MakeTempFilePath("unsupported.png");
    EXPECT_FALSE(img.WritePNG({path.string(), false}));
}
