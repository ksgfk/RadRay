#include <radray/rhi/common.h>

namespace radray::rhi {

const char* to_string(ApiType val) noexcept {
    switch (val) {
        case radray::rhi::ApiType::D3D12: return "D3D12";
        case radray::rhi::ApiType::Metal: return "Metal";
        case radray::rhi::ApiType::MAX_COUNT: return "UNKNOWN";
    }
}

const char* to_string(PixelFormat val) noexcept {
    switch (val) {
        case PixelFormat::Unknown: return "Unknown";
        case PixelFormat::R8_SInt: return "R8_SInt";
        case PixelFormat::R8_UInt: return "R8_UInt";
        case PixelFormat::R8_UNorm: return "R8_UNorm";
        case PixelFormat::RG8_SInt: return "RG8_SInt";
        case PixelFormat::RG8_UInt: return "RG8_UInt";
        case PixelFormat::RG8_UNorm: return "RG8_UNorm";
        case PixelFormat::RGBA8_SInt: return "RGBA8_SInt";
        case PixelFormat::RGBA8_UInt: return "RGBA8_UInt";
        case PixelFormat::RGBA8_UNorm: return "RGBA8_UNorm";
        case PixelFormat::R16_SInt: return "R16_SInt";
        case PixelFormat::R16_UInt: return "R16_UInt";
        case PixelFormat::R16_UNorm: return "R16_UNorm";
        case PixelFormat::RG16_SInt: return "RG16_SInt";
        case PixelFormat::RG16_UInt: return "RG16_UInt";
        case PixelFormat::RG16_UNorm: return "RG16_UNorm";
        case PixelFormat::RGBA16_SInt: return "RGBA16_SInt";
        case PixelFormat::RGBA16_UInt: return "RGBA16_UInt";
        case PixelFormat::RGBA16_UNorm: return "RGBA16_UNorm";
        case PixelFormat::R32_SInt: return "R32_SInt";
        case PixelFormat::R32_UInt: return "R32_UInt";
        case PixelFormat::RG32_SInt: return "RG32_SInt";
        case PixelFormat::RG32_UInt: return "RG32_UInt";
        case PixelFormat::RGBA32_SInt: return "RGBA32_SInt";
        case PixelFormat::RGBA32_UInt: return "RGBA32_UInt";
        case PixelFormat::R16_Float: return "R16_Float";
        case PixelFormat::RG16_Float: return "RG16_Float";
        case PixelFormat::RGBA16_Float: return "RGBA16_Float";
        case PixelFormat::R32_Float: return "R32_Float";
        case PixelFormat::RG32_Float: return "RG32_Float";
        case PixelFormat::RGBA32_Float: return "RGBA32_Float";
        case PixelFormat::R10G10B10A2_UInt: return "R10G10B10A2_UInt";
        case PixelFormat::R10G10B10A2_UNorm: return "R10G10B10A2_UNorm";
        case PixelFormat::R11G11B10_Float: return "R11G11B10_Float";
        case PixelFormat::D16_UNorm: return "D16_UNorm";
        case PixelFormat::D32_Float: return "D32_Float";
        case PixelFormat::D24S8: return "D24S8";
        case PixelFormat::D32S8: return "D32S8";
        default: return "Unknown";
    }
}

const char* to_string(TextureDimension val) noexcept {
    switch (val) {
        case TextureDimension::Tex_1D: return "Tex_1D";
        case TextureDimension::Tex_2D: return "Tex_2D";
        case TextureDimension::Tex_3D: return "Tex_3D";
        case TextureDimension::Cubemap: return "Cubemap";
        case TextureDimension::Tex_2D_Array: return "Tex_2D_Array";
        default: return "Unknown";
    }
}

const char* to_string(BufferType val) noexcept {
    switch (val) {
        case BufferType::Default: return "Default";
        case BufferType::Upload: return "Upload";
        case BufferType::Readback: return "Readback";
        default: return "Unknown";
    }
}

}  // namespace radray::rhi
