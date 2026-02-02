#define _CRT_SECURE_NO_WARNINGS

#include <fstream>
#include <filesystem>
#include <cstdlib>

#include <gtest/gtest.h>
#include <radray/image_data.h>

static std::filesystem::path GetAssetsPath() {
    if (auto env = std::getenv("RADRAY_ASSETS_DIR")) {
        return std::filesystem::path{env};
    }
    return "assets";
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
