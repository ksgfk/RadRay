#pragma once

#include <optional>

#include <radray/logger.h>

namespace radray::rhi {

enum class ApiType {
    D3D12,
    Metal,
    MAX_COUNT
};

enum class CommandListType {
    Graphics,
    Compute
};

enum class BufferType {
    Default,
    Upload,
    Readback
};

enum class Format {
    Unknown,
    R32G32B32A32_Typeless,
    R32G32B32A32_Float,
    R32G32B32A32_UInt,
    R32G32B32A32_SInt,
    R32G32B32_Typeless,
    R32G32B32_Float,
    R32G32B32_UInt,
    R32G32B32_SInt,
    R16G16B16A16_Typeless,
    R16G16B16A16_Float,
    R16G16B16A16_UNorm,
    R16G16B16A16_UInt,
    R16G16B16A16_SNorm,
    R16G16B16A16_SInt,
    R32G32_Typeless,
    R32G32_Float,
    R32G32_UInt,
    R32G32_SInt,
    R32G8X24_Typeless,
    D32_Float_S8X24_UInt,
    R32_Float_X8X24_Typeless,
    X32_Typeless_G8X24_UInt,
    R10G10B10A2_Typeless,
    R10G10B10A2_UNorm,
    R10G10B10A2_UInt,
    R11G11B10_Float,
    R8G8B8A8_Typeless,
    R8G8B8A8_UNorm,
    R8G8B8A8_UNorm_SRGB,
    R8G8B8A8_UInt,
    R8G8B8A8_SNorm,
    R8G8B8A8_SInt,
    R16G16_Typeless,
    R16G16_Float,
    R16G16_UNorm,
    R16G16_UInt,
    R16G16_SNorm,
    R16G16_SInt,
    R32_Typeless,
    D32_Float,
    R32_Float,
    R32_UInt,
    R32_SInt,
    R24G8_Typeless,
    D24_UNorm_S8_UInt,
    R24_UNorm_X8_Typeless,
    X24_Typeless_G8_UInt,
    R8G8_Typeless,
    R8G8_UNorm,
    R8G8_UInt,
    R8G8_SNorm,
    R8G8_SInt,
    R16_Typeless,
    R16_Float,
    D16_UNorm,
    R16_UNorm,
    R16_UInt,
    R16_SNorm,
    R16_SInt,
    R8_Typeless,
    R8_UNorm,
    R8_UInt,
    R8_SNorm,
    R8_SInt,
    A8_UNorm,
    R1_UNorm,
    R9G9B9E5_SharedExp,
    R8G8_B8G8_UNorm,
    G8R8_G8B8_UNorm,
    BC1_Typeless,
    BC1_UNorm,
    BC1_UNorm_SRGB,
    BC2_Typeless,
    BC2_UNorm,
    BC2_UNorm_SRGB,
    BC3_Typeless,
    BC3_UNorm,
    BC3_UNorm_SRGB,
    BC4_Typeless,
    BC4_UNorm,
    BC4_SNorm,
    BC5_Typeless,
    BC5_UNorm,
    BC5_SNorm,
    B5G6R5_UNorm,
    B5G5R5A1_UNorm,
    B8G8R8A8_UNorm,
    B8G8R8X8_UNorm,
    R10G10B10_XR_BIAS_A2_UNorm,
    B8G8R8A8_Typeless,
    B8G8R8A8_UNorm_SRGB,
    B8G8R8X8_Typeless,
    B8G8R8X8_UNorm_SRGB,
    BC6H_Typeless,
    BC6H_UF16,
    BC6H_SF16,
    BC7_Typeless,
    BC7_UNorm,
    BC7_UNorm_SRGB,
    AYUV,
    Y410,
    Y416,
    NV12,
    P010,
    P016,
    _420_OPAQUE,
    YUY2,
    Y210,
    Y216,
    NV11,
    AI44,
    IA44,
    P8,
    A8P8,
    B4G4R4A4_UNorm,
    P208,
    V208,
    V408,
    Sampler_FeedBack_Min_Mip_Opaque,
    Sampler_FeedBack_Mip_region_Used_Opaque,
    Force_UInt,
};

enum class TextureDimension {
    Unknown,
    Tex1D,
    Tex2D,
    Tex3D,
    CubeMap
};

struct DeviceCreateInfoD3D12 {
    std::optional<uint32_t> AdapterIndex;
    bool IsEnableDebugLayer;
};

struct DeviceCreateInfoMetal {
    uint32_t DeviceIndex;
};

struct SwapChainCreateInfo {
};

struct CommandQueueCreateInfo {
    CommandListType Type;
};

struct FenceCreateInfo {
};

struct BufferCreateInfo {
    BufferType type;
    uint64_t byteSize;
};

struct TextureCreateInfo {
};

struct ShaderCreateInfo {
};

}  // namespace radray::rhi

template <>
struct std::formatter<radray::rhi::ApiType> : std::formatter<const char*> {
    auto format(radray::rhi::ApiType const& val, format_context& ctx) const -> decltype(ctx.out());
};
