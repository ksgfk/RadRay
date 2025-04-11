#include <radray/render/tool_utility.h>

namespace radray::render {

TextureFormat ImageToTextureFormat(radray::ImageFormat fmt) noexcept {
    switch (fmt) {
        case ImageFormat::R8_BYTE: return TextureFormat::R8_UNORM;
        case ImageFormat::R16_USHORT: return TextureFormat::R16_UINT;
        case ImageFormat::R16_HALF: return TextureFormat::R16_FLOAT;
        case ImageFormat::R32_FLOAT: return TextureFormat::R32_FLOAT;
        case ImageFormat::RG8_BYTE: return TextureFormat::RG8_UNORM;
        case ImageFormat::RG16_USHORT: return TextureFormat::RG16_UINT;
        case ImageFormat::RG16_HALF: return TextureFormat::RG16_FLOAT;
        case ImageFormat::RG32_FLOAT: return TextureFormat::RG32_FLOAT;
        case ImageFormat::RGB32_FLOAT: return TextureFormat::UNKNOWN;
        case ImageFormat::RGBA8_BYTE: return TextureFormat::RGBA8_UNORM;
        case ImageFormat::RGBA16_USHORT: return TextureFormat::RGBA16_UINT;
        case ImageFormat::RGBA16_HALF: return TextureFormat::RGBA16_FLOAT;
        case ImageFormat::RGBA32_FLOAT: return TextureFormat::RGBA32_FLOAT;
        case ImageFormat::RGB8_BYTE: return TextureFormat::UNKNOWN;
        case ImageFormat::RGB16_USHORT: return TextureFormat::UNKNOWN;
    }
}

}  // namespace radray::render