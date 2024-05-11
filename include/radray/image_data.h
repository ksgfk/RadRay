#pragma once

#include <memory>
#include <span>

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

class ImageData {
public:
    size_t GetSize() const noexcept;
    std::span<const byte> GetSpan() const noexcept;

    static size_t FormatByteSize(ImageFormat format) noexcept;

    std::unique_ptr<byte[]> data;
    uint32_t width;
    uint32_t height;
    ImageFormat format;
};

}  // namespace radray

template<>
struct std::formatter<radray::ImageFormat> : public std::formatter<const char*> {
    auto format(radray::ImageFormat const& val, std::format_context& ctx) const -> decltype(ctx.out());
};
