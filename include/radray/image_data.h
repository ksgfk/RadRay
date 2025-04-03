#pragma once

#include <span>
#include <string_view>
#include <optional>
#include <istream>

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

enum class ImageFormat {
    R8_BYTE,       // 1
    R16_USHORT,    // 2
    R16_HALF,      // 2
    R32_FLOAT,     // 4

    RG8_BYTE,      // 2
    RG16_USHORT,   // 4
    RG16_HALF,     // 4
    RG32_FLOAT,    // 8

    RGB32_FLOAT,   // 12

    RGBA8_BYTE,    // 4
    RGBA16_USHORT, // 8
    RGBA16_HALF,   // 8
    RGBA32_FLOAT,  // 16

    RGB8_BYTE,     // 3
    RGB16_USHORT   // 6
};

size_t GetImageFormatSize(ImageFormat format) noexcept;

class ImageData {
public:
    size_t GetSize() const noexcept;
    std::span<const byte> GetSpan() const noexcept;

    radray::unique_ptr<byte[]> Data;
    uint32_t Width;
    uint32_t Height;
    ImageFormat Format;
};

#ifdef RADRAY_ENABLE_PNG
bool IsPNG(std::istream& stream);
std::optional<ImageData> LoadPNG(std::istream& stream);
#endif

std::string_view to_string(ImageFormat val) noexcept;

}  // namespace radray

template <class CharT>
struct fmt::formatter<radray::ImageFormat, CharT> : fmt::formatter<std::string_view, CharT> {
    template <class FormatContext>
    auto format(radray::ImageFormat val, FormatContext& ctx) const {
        return formatter<std::string_view, CharT>::format(to_string(val), ctx);
    }
};
