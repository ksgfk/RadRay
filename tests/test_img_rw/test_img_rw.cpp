#include <fstream>
#include <filesystem>

#include <gtest/gtest.h>
#include <radray/image_data.h>

TEST(PNG, SimpleLoad) {
    auto filename = std::filesystem::path{"assets"} / "1735141462310.png";
    std::ifstream file{filename, std::ios::binary};
    ASSERT_TRUE(file.is_open());
    auto result = radray::LoadPNG(file);
    ASSERT_TRUE(result.has_value());
    radray::ImageData img = std::move(result.value());
    ASSERT_EQ(img.Width, 2560);
    ASSERT_EQ(img.Height, 1440);
}

TEST(PNG, ToRGBA8) {
    auto filename = std::filesystem::path{"assets"} / "1735141462310.png";
    std::ifstream file{filename, std::ios::binary};
    ASSERT_TRUE(file.is_open());
    auto result = radray::LoadPNG(file);
    ASSERT_TRUE(result.has_value());
    radray::ImageData img = std::move(result.value());
    ASSERT_EQ(img.Width, 2560);
    ASSERT_EQ(img.Height, 1440);

    radray::ImageData rgba = img.RGB8ToRGBA8(0xff);
    ASSERT_EQ(rgba.Width, 2560);
    ASSERT_EQ(rgba.Height, 1440);
    ASSERT_EQ(rgba.Format, radray::ImageFormat::RGBA8_BYTE);
}
