#include <radray/render/common.h>

#include <radray/logger.h>

#ifdef RADRAY_ENABLE_D3D12
#include "d3d12/d3d12_impl.h"
#endif

#ifdef RADRAY_ENABLE_METAL
#include "metal/metal_device.h
#endif

#ifdef RADRAY_ENABLE_VULKAN
#include "vk/vulkan_impl.h"
#endif

namespace radray::render {

bool GlobalInitGraphics(std::span<BackendInitDescriptor> descs) {
    for (const auto& desc : descs) {
        bool isSucc = std::visit(
            [](auto&& arg) -> bool {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, VulkanBackendInitDescriptor>) {
#ifdef RADRAY_ENABLE_VULKAN
                    return vulkan::GlobalInitVulkan(arg);
#else
                    RADRAY_ERR_LOG("Vulkan is not enabled");
                    return false;
#endif
                } else if constexpr (std::is_same_v<T, D3D12BackendInitDescriptor>) {
                    return false;
                } else if constexpr (std::is_same_v<T, MetalBackendInitDescriptor>) {
                    return false;
                } else {
                    return false;
                }
            },
            desc);
        if (!isSucc) {
            return false;
        }
    }
    return true;
}

void GlobalTerminateGraphics() {
#ifdef RADRAY_ENABLE_VULKAN
    vulkan::GlobalTerminateVulkan();
#endif
}

Nullable<shared_ptr<Device>> CreateDevice(const DeviceDescriptor& desc) {
    return std::visit(
        [](auto&& arg) -> Nullable<shared_ptr<Device>> {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, D3D12DeviceDescriptor>) {
#ifdef RADRAY_ENABLE_D3D12
                return d3d12::CreateDevice(arg);
#else
                RADRAY_ERR_LOG("D3D12 is not enable");
                return nullptr;
#endif
            } else if constexpr (std::is_same_v<T, MetalDeviceDescriptor>) {
#ifdef RADRAY_ENABLE_METAL
                return metal::CreateDevice(arg);
#else
                RADRAY_ERR_LOG("Metal is not enable");
                return nullptr;
#endif
            } else if constexpr (std::is_same_v<T, VulkanDeviceDescriptor>) {
#ifdef RADRAY_ENABLE_VULKAN
                return vulkan::CreateDeviceVulkan(arg);
#else
                RADRAY_ERR_LOG("Vulkan is not enable");
                return nullptr;
#endif
            }
        },
        desc);
}

bool operator==(const SamplerDescriptor& lhs, const SamplerDescriptor& rhs) noexcept {
    return lhs.AddressS == rhs.AddressS &&
           lhs.AddressT == rhs.AddressT &&
           lhs.AddressR == rhs.AddressR &&
           lhs.MigFilter == rhs.MigFilter &&
           lhs.MagFilter == rhs.MagFilter &&
           lhs.MipmapFilter == rhs.MipmapFilter &&
           lhs.LodMin == rhs.LodMin &&
           lhs.LodMax == rhs.LodMax &&
           lhs.Compare == rhs.Compare &&
           lhs.AnisotropyClamp == rhs.AnisotropyClamp;
}

bool operator!=(const SamplerDescriptor& lhs, const SamplerDescriptor& rhs) noexcept {
    return !(lhs == rhs);
}

bool operator==(const StaticSamplerDescriptor& lhs, const StaticSamplerDescriptor& rhs) noexcept {
    return operator==(static_cast<const SamplerDescriptor&>(lhs), static_cast<const SamplerDescriptor&>(rhs)) &&
           lhs.Slot == rhs.Slot &&
           lhs.Space == rhs.Space &&
           lhs.Stages == rhs.Stages;
}

bool operator!=(const StaticSamplerDescriptor& lhs, const StaticSamplerDescriptor& rhs) noexcept {
    return !(lhs == rhs);
}

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
        case VertexFormat::UNKNOWN:
        default: return 0;
    }
}

PrimitiveState DefaultPrimitiveState() noexcept {
    return {
        PrimitiveTopology::TriangleList,
        FrontFace::CW,
        CullMode::Back,
        PolygonMode::Fill,
        std::nullopt,
        true,
        false};
}

DepthStencilState DefaultDepthStencilState() noexcept {
    return {
        TextureFormat::D24_UNORM_S8_UINT,
        CompareFunction::Less,
        {
            0,
            0.0f,
            0.0f,
        },
        std::nullopt,
        true};
}

StencilState DefaultStencilState() noexcept {
    return {
        {
            CompareFunction::Always,
            StencilOperation::Keep,
            StencilOperation::Keep,
            StencilOperation::Keep,
        },
        {
            CompareFunction::Always,
            StencilOperation::Keep,
            StencilOperation::Keep,
            StencilOperation::Keep,
        },
        0xFF,
        0xFF};
}

MultiSampleState DefaultMultiSampleState() noexcept {
    return {
        .Count = 1,
        .Mask = 0xFFFFFFFF,
        .AlphaToCoverageEnable = false};
}

ColorTargetState DefaultColorTargetState(TextureFormat format) noexcept {
    return {
        format,
        std::nullopt,
        ColorWrite::All};
}

BlendState DefaultBlendState() noexcept {
    return {
        {BlendFactor::One,
         BlendFactor::Zero,
         BlendOperation::Add},
        {BlendFactor::One,
         BlendFactor::Zero,
         BlendOperation::Add}};
}

std::string_view format_as(Backend v) noexcept {
    switch (v) {
        case Backend::D3D12: return "D3D12";
        case Backend::Vulkan: return "Vulkan";
        case Backend::Metal: return "Metal";
        default: return "UNKNOWN";
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
        default: return "UNKNOWN";
    }
}

std::string_view format_as(QueueType v) noexcept {
    switch (v) {
        case radray::render::QueueType::Direct: return "Direct";
        case radray::render::QueueType::Compute: return "Compute";
        case radray::render::QueueType::Copy: return "Copy";
        default: return "UNKNOWN";
    }
}

std::string_view format_as(ShaderBlobCategory v) noexcept {
    switch (v) {
        case ShaderBlobCategory::DXIL: return "DXIL";
        case ShaderBlobCategory::SPIRV: return "SPIR-V";
        case ShaderBlobCategory::MSL: return "MSL";
        default: return "UNKNOWN";
    }
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
        default: return "UNKNOWN";
    }
}

std::string_view format_as(PolygonMode v) noexcept {
    switch (v) {
        case PolygonMode::Fill: return "Fill";
        case PolygonMode::Line: return "Line";
        case PolygonMode::Point: return "Point";
        default: return "UNKNOWN";
    }
}

std::string_view format_as(TextureViewDimension v) noexcept {
    switch (v) {
        case TextureViewDimension::UNKNOWN: return "UNKNOWN";
        case TextureViewDimension::Dim1D: return "1D";
        case TextureViewDimension::Dim2D: return "2D";
        case TextureViewDimension::Dim3D: return "3D";
        case TextureViewDimension::Dim1DArray: return "1DArray";
        case TextureViewDimension::Dim2DArray: return "2DArray";
        case TextureViewDimension::Cube: return "Cube";
        case TextureViewDimension::CubeArray: return "CubeArray";
        default: return "UNKNOWN";
    }
}

std::string_view format_as(ResourceBindType v) noexcept {
    switch (v) {
        case ResourceBindType::CBuffer: return "CBuffer";
        case ResourceBindType::Buffer: return "Buffer";
        case ResourceBindType::RWBuffer: return "RWBuffer";
        case ResourceBindType::Texture: return "Texture";
        case ResourceBindType::RWTexture: return "RWTexture";
        case ResourceBindType::Sampler: return "Sampler";
        case ResourceBindType::UNKNOWN:
        default: return "UNKNOWN";
    }
}

std::string_view format_as(RenderObjectTag v) noexcept {
    switch (v) {
        case RenderObjectTag::UNKNOWN: return "UNKNOWN";
        case RenderObjectTag::Device: return "Device";
        case RenderObjectTag::CmdQueue: return "CmdQueue";
        case RenderObjectTag::CmdBuffer: return "CmdBuffer";
        case RenderObjectTag::CmdEncoder: return "CmdEncoder";
        case RenderObjectTag::Fence: return "Fence";
        case RenderObjectTag::Shader: return "Shader";
        case RenderObjectTag::RootSignature: return "RootSignature";
        case RenderObjectTag::PipelineState: return "PipelineState";
        case RenderObjectTag::GraphicsPipelineState: return "GraphicsPipelineState";
        case RenderObjectTag::SwapChain: return "SwapChain";
        case RenderObjectTag::Resource: return "Resource";
        case RenderObjectTag::Buffer: return "Buffer";
        case RenderObjectTag::Texture: return "Texture";
        case RenderObjectTag::ResourceView: return "ResourceView";
        case RenderObjectTag::BufferView: return "BufferView";
        case RenderObjectTag::TextureView: return "TextureView";
        case RenderObjectTag::DescriptorSet: return "DescriptorSet";
        case RenderObjectTag::DescriptorSetLayout: return "DescriptorSetLayout";
        case RenderObjectTag::Sampler: return "Sampler";
        default: return "UNKNOWN";
    }
}

}  // namespace radray::render
