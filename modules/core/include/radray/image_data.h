#pragma once

#include <span>
#include <string_view>
#include <optional>
#include <istream>

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

enum class ImageFormat {
    R8_BYTE,     // 1
    R16_USHORT,  // 2
    R16_HALF,    // 2
    R32_FLOAT,   // 4

    RG8_BYTE,     // 2
    RG16_USHORT,  // 4
    RG16_HALF,    // 4
    RG32_FLOAT,   // 8

    RGB32_FLOAT,  // 12

    RGBA8_BYTE,     // 4
    RGBA16_USHORT,  // 8
    RGBA16_HALF,    // 8
    RGBA32_FLOAT,   // 16

    RGB8_BYTE,    // 3
    RGB16_USHORT  // 6
};

size_t GetImageFormatSize(ImageFormat format) noexcept;

class ImageData {
public:
    ImageData() noexcept = default;
    ImageData(const ImageData&) noexcept;
    ImageData(ImageData&&) noexcept;
    ImageData& operator=(const ImageData&) noexcept;
    ImageData& operator=(ImageData&&) noexcept;
    ~ImageData() noexcept = default;

    size_t GetSize() const noexcept;
    std::span<const byte> GetSpan() const noexcept;

    ImageData RGB8ToRGBA8(uint8_t alpha) const noexcept;
    void FlipY() noexcept;

    unique_ptr<byte[]> Data{};
    uint32_t Width{0};
    uint32_t Height{0};
    ImageFormat Format{ImageFormat::R8_BYTE};
};

#ifdef RADRAY_ENABLE_PNG
class PNGLoadSettings {
public:
    std::optional<uint32_t> AddAlphaIfRGB;
    bool IsFlipY{false};
};
bool IsPNG(std::istream& stream);
std::optional<ImageData> LoadPNG(std::istream& stream, PNGLoadSettings settings = PNGLoadSettings{});
#endif

std::string_view format_as(ImageFormat val) noexcept;

}  // namespace radray
