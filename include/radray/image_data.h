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

const char* to_string(ImageFormat val) noexcept;

}  // namespace radray

template <class CharT>
struct std::formatter<radray::ImageFormat, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::ImageFormat val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};
