#include <radray/render/common.h>

namespace radray::render {

bool IsDepthStencilFormat(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::S8: return "S8";
        case TextureFormat::D16_UNORM: return "D16_UNORM";
        case TextureFormat::D32_FLOAT: return "D32_FLOAT";
        case TextureFormat::D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case TextureFormat::D32_FLOAT_S8_UINT: return true; return "D32_FLOAT_S8_UINT: return true";
        default: return false;
    }
}

std::string_view format_as(Backend v) noexcept {
    switch (v) {
        case Backend::D3D12: return "D3D12";
        case Backend::Vulkan: return "Vulkan";
        case Backend::Metal: return "Metal";
    }
}

std::string_view format_as(TextureFormat v) noexcept {
    switch (v) {
        case TextureFormat::UNKNOWN: return "UNKNOWN";
        case TextureFormat::R8_SINT: return "R8_SINT";
        case TextureFormat::R8_UINT: return "R8_UINT";
        case TextureFormat::R8_SNORM: return "R8_SNORM";
        case TextureFormat::R8_UNORM: return "R8_UNORM";
        case TextureFormat::R16_SINT: return "R16_SINT";
        case TextureFormat::R16_UINT: return "R16_UINT";
        case TextureFormat::R16_SNORM: return "R16_SNORM";
        case TextureFormat::R16_UNORM: return "R16_UNORM";
        case TextureFormat::R16_FLOAT: return "R16_FLOAT";
        case TextureFormat::RG8_SINT: return "RG8_SINT";
        case TextureFormat::RG8_UINT: return "RG8_UINT";
        case TextureFormat::RG8_SNORM: return "RG8_SNORM";
        case TextureFormat::RG8_UNORM: return "RG8_UNORM";
        case TextureFormat::R32_SINT: return "R32_SINT";
        case TextureFormat::R32_UINT: return "R32_UINT";
        case TextureFormat::R32_FLOAT: return "R32_FLOAT";
        case TextureFormat::RG16_SINT: return "RG16_SINT";
        case TextureFormat::RG16_UINT: return "RG16_UINT";
        case TextureFormat::RG16_SNORM: return "RG16_SNORM";
        case TextureFormat::RG16_UNORM: return "RG16_UNORM";
        case TextureFormat::RG16_FLOAT: return "RG16_FLOAT";
        case TextureFormat::RGBA8_SINT: return "RGBA8_SINT";
        case TextureFormat::RGBA8_UINT: return "RGBA8_UINT";
        case TextureFormat::RGBA8_SNORM: return "RGBA8_SNORM";
        case TextureFormat::RGBA8_UNORM: return "RGBA8_UNORM";
        case TextureFormat::RGBA8_UNORM_SRGB: return "RGBA8_UNORM_SRGB";
        case TextureFormat::BGRA8_UNORM: return "BGRA8_UNORM";
        case TextureFormat::BGRA8_UNORM_SRGB: return "BGRA8_UNORM_SRGB";
        case TextureFormat::RGB10A2_UINT: return "RGB10A2_UINT";
        case TextureFormat::RGB10A2_UNORM: return "RGB10A2_UNORM";
        case TextureFormat::RG11B10_FLOAT: return "RG11B10_FLOAT";
        case TextureFormat::RG32_SINT: return "RG32_SINT";
        case TextureFormat::RG32_UINT: return "RG32_UINT";
        case TextureFormat::RG32_FLOAT: return "RG32_FLOAT";
        case TextureFormat::RGBA16_SINT: return "RGBA16_SINT";
        case TextureFormat::RGBA16_UINT: return "RGBA16_UINT";
        case TextureFormat::RGBA16_SNORM: return "RGBA16_SNORM";
        case TextureFormat::RGBA16_UNORM: return "RGBA16_UNORM";
        case TextureFormat::RGBA16_FLOAT: return "RGBA16_FLOAT";
        case TextureFormat::RGBA32_SINT: return "RGBA32_SINT";
        case TextureFormat::RGBA32_UINT: return "RGBA32_UINT";
        case TextureFormat::RGBA32_FLOAT: return "RGBA32_FLOAT";
        case TextureFormat::S8: return "S8";
        case TextureFormat::D16_UNORM: return "D16_UNORM";
        case TextureFormat::D32_FLOAT: return "D32_FLOAT";
        case TextureFormat::D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case TextureFormat::D32_FLOAT_S8_UINT: return "D32_FLOAT_S8_UINT";
    }
}

std::string_view format_as(QueueType v) noexcept {
    switch (v) {
        case radray::render::QueueType::Direct: return "Direct";
        case radray::render::QueueType::Compute: return "Compute";
        case radray::render::QueueType::Copy: return "Copy";
    }
}

}  // namespace radray::render
