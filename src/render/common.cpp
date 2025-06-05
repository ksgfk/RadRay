#include <radray/render/common.h>

#include <radray/utility.h>

namespace radray::render {

bool IsDepthStencilFormat(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::S8:
        case TextureFormat::D16_UNORM:
        case TextureFormat::D32_FLOAT:
        case TextureFormat::D24_UNORM_S8_UINT:
        case TextureFormat::D32_FLOAT_S8_UINT: return true;
        default: return false;
    }
}

uint32_t GetVertexFormatSize(VertexFormat format) noexcept {
    switch (format) {
        case VertexFormat::UINT8X2:
        case VertexFormat::SINT8X2:
        case VertexFormat::UNORM8X2:
        case VertexFormat::SNORM8X2: return 2;
        case VertexFormat::UINT8X4:
        case VertexFormat::SINT8X4:
        case VertexFormat::UNORM8X4:
        case VertexFormat::SNORM8X4:
        case VertexFormat::UINT16X2:
        case VertexFormat::SINT16X2:
        case VertexFormat::UNORM16X2:
        case VertexFormat::SNORM16X2:
        case VertexFormat::FLOAT16X2:
        case VertexFormat::UINT32:
        case VertexFormat::SINT32:
        case VertexFormat::FLOAT32: return 4;
        case VertexFormat::UINT16X4:
        case VertexFormat::SINT16X4:
        case VertexFormat::UNORM16X4:
        case VertexFormat::SNORM16X4:
        case VertexFormat::FLOAT16X4:
        case VertexFormat::UINT32X2:
        case VertexFormat::SINT32X2:
        case VertexFormat::FLOAT32X2: return 8;
        case VertexFormat::UINT32X3:
        case VertexFormat::SINT32X3:
        case VertexFormat::FLOAT32X3: return 12;
        case VertexFormat::UINT32X4:
        case VertexFormat::SINT32X4:
        case VertexFormat::FLOAT32X4: return 16;
        case VertexFormat::UNKNOWN: return 0;
    }
    Unreachable();
}

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
    Unreachable();
}

std::string_view format_as(Backend v) noexcept {
    switch (v) {
        case Backend::D3D12: return "D3D12";
        case Backend::Vulkan: return "Vulkan";
        case Backend::Metal: return "Metal";
    }
    Unreachable();
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
    Unreachable();
}

std::string_view format_as(QueueType v) noexcept {
    switch (v) {
        case radray::render::QueueType::Direct: return "Direct";
        case radray::render::QueueType::Compute: return "Compute";
        case radray::render::QueueType::Copy: return "Copy";
    }
    Unreachable();
}

std::string_view format_as(ShaderBlobCategory v) noexcept {
    switch (v) {
        case ShaderBlobCategory::DXIL: return "DXIL";
        case ShaderBlobCategory::SPIRV: return "SPIR-V";
        case ShaderBlobCategory::MSL: return "MSL";
    }
    Unreachable();
}

std::string_view format_as(ShaderResourceType v) noexcept {
    switch (v) {
        case ShaderResourceType::CBuffer: return "CBuffer";
        case ShaderResourceType::Texture: return "Texture";
        case ShaderResourceType::Buffer: return "Buffer";
        case ShaderResourceType::RWTexture: return "RWTexture";
        case ShaderResourceType::RWBuffer: return "RWBuffer";
        case ShaderResourceType::Sampler: return "Sampler";
        case ShaderResourceType::PushConstant: return "PushConstant";
        case ShaderResourceType::RayTracing: return "RayTracing";
    }
    Unreachable();
}

std::string_view format_as(VertexFormat v) noexcept {
    switch (v) {
        case VertexFormat::UNKNOWN: return "UNKNOWN";
        case VertexFormat::UINT8X2: return "byte2";
        case VertexFormat::UINT8X4: return "byte4";
        case VertexFormat::SINT8X2: return "char2";
        case VertexFormat::SINT8X4: return "char4";
        case VertexFormat::UNORM8X2: return "unorm8x2";
        case VertexFormat::UNORM8X4: return "unorm8x4";
        case VertexFormat::SNORM8X2: return "snorm8x2";
        case VertexFormat::SNORM8X4: return "snorm8x4";
        case VertexFormat::UINT16X2: return "ushort2";
        case VertexFormat::UINT16X4: return "ushort4";
        case VertexFormat::SINT16X2: return "short2";
        case VertexFormat::SINT16X4: return "short4";
        case VertexFormat::UNORM16X2: return "unorm16x2";
        case VertexFormat::UNORM16X4: return "unorm16x4";
        case VertexFormat::SNORM16X2: return "snorm16x2";
        case VertexFormat::SNORM16X4: return "snorm16x4";
        case VertexFormat::FLOAT16X2: return "half2";
        case VertexFormat::FLOAT16X4: return "half4";
        case VertexFormat::UINT32: return "uint";
        case VertexFormat::UINT32X2: return "uint2";
        case VertexFormat::UINT32X3: return "uint3";
        case VertexFormat::UINT32X4: return "uint4";
        case VertexFormat::SINT32: return "int";
        case VertexFormat::SINT32X2: return "int2";
        case VertexFormat::SINT32X3: return "int3";
        case VertexFormat::SINT32X4: return "int4";
        case VertexFormat::FLOAT32: return "float";
        case VertexFormat::FLOAT32X2: return "float2";
        case VertexFormat::FLOAT32X3: return "float3";
        case VertexFormat::FLOAT32X4: return "float4";
    }
    Unreachable();
}

std::string_view format_as(PolygonMode v) noexcept {
    switch (v) {
        case PolygonMode::Fill: return "Fill";
        case PolygonMode::Line: return "Line";
        case PolygonMode::Point: return "Point";
    }
    Unreachable();
}

std::string_view format_as(ResourceType v) noexcept {
    switch (v) {
        case ResourceType::UNKNOWN: return "UNKNOWN";
        case ResourceType::Sampler: return "Sampler";
        case ResourceType::Texture: return "Texture";
        case ResourceType::RenderTarget: return "RenderTarget";
        case ResourceType::DepthStencil: return "DepthStencil";
        case ResourceType::TextureRW: return "TextureRW";
        case ResourceType::Buffer: return "Buffer";
        case ResourceType::CBuffer: return "CBuffer";
        case ResourceType::PushConstant: return "PushConstant";
        case ResourceType::BufferRW: return "BufferRW";
        case ResourceType::RayTracing: return "RayTracing";
    }
    Unreachable();
}

std::string_view format_as(ResourceUsage v) noexcept {
    switch (v) {
        case ResourceUsage::Default: return "Default";
        case ResourceUsage::Upload: return "Upload";
        case ResourceUsage::Readback: return "Readback";
    }
    Unreachable();
}

std::string_view format_as(TextureDimension v) noexcept {
    switch (v) {
        case TextureDimension::UNKNOWN: return "UNKNOWN";
        case TextureDimension::Dim1D: return "1D";
        case TextureDimension::Dim2D: return "2D";
        case TextureDimension::Dim3D: return "3D";
        case TextureDimension::Dim1DArray: return "1DArray";
        case TextureDimension::Dim2DArray: return "2DArray";
        case TextureDimension::Cube: return "Cube";
        case TextureDimension::CubeArray: return "CubeArray";
    }
    Unreachable();
}

}  // namespace radray::render
