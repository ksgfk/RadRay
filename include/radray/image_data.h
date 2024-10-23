#pragma once

#include <span>
#include <string_view>

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

enum class ImageFormat {
    R8_BYTE,
    R16_USHORT,
    R16_HALF,
    R32_FLOAT,

    RG8_BYTE,
    RG16_USHORT,
    RG16_HALF,
    RG32_FLOAT,

    RGB32_FLOAT,

    RGBA8_BYTE,
    RGBA16_USHORT,
    RGBA16_HALF,
    RGBA32_FLOAT,
};

size_t GetImageFormatSize(ImageFormat format) noexcept;

class ImageData {
public:
    size_t GetSize() const noexcept;
    std::span<const byte> GetSpan() const noexcept;

    radray::unique_ptr<byte[]> data;
    uint32_t width;
    uint32_t height;
    ImageFormat format;
};

std::string_view to_string(ImageFormat val) noexcept;

}  // namespace radray

template <class CharT>
struct fmt::formatter<radray::ImageFormat, CharT> : fmt::formatter<std::string_view, CharT> {
    template <class FormatContext>
    auto format(radray::ImageFormat val, FormatContext& ctx) const {
        return formatter<std::string_view, CharT>::format(to_string(val), ctx);
    }
};
