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

enum class PixelFormat : uint32_t {
    Unknown,
    R8_SInt,
    R8_UInt,
    R8_UNorm,
    RG8_SInt,
    RG8_UInt,
    RG8_UNorm,
    RGBA8_SInt,
    RGBA8_UInt,
    RGBA8_UNorm,
    R16_SInt,
    R16_UInt,
    R16_UNorm,
    RG16_SInt,
    RG16_UInt,
    RG16_UNorm,
    RGBA16_SInt,
    RGBA16_UInt,
    RGBA16_UNorm,
    R32_SInt,
    R32_UInt,
    RG32_SInt,
    RG32_UInt,
    RGBA32_SInt,
    RGBA32_UInt,
    R16_Float,
    RG16_Float,
    RGBA16_Float,
    R32_Float,
    RG32_Float,
    RGBA32_Float,
    R10G10B10A2_UInt,
    R10G10B10A2_UNorm,
    R11G11B10_Float
};

struct DeviceCreateInfoD3D12 {
    std::optional<uint32_t> AdapterIndex;
    bool IsEnableDebugLayer;
};

struct DeviceCreateInfoMetal {
    uint32_t DeviceIndex;
};

struct SwapChainCreateInfo {
    uint64_t WindowHandle;
    uint32_t Width;
    uint32_t Height;
    uint32_t BackBufferCount;
    bool Vsync;
};

const char* to_string(ApiType val) noexcept;
const char* to_string(PixelFormat val) noexcept;

}  // namespace radray::rhi

template <class CharT>
struct std::formatter<radray::rhi::ApiType, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::ApiType val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::rhi::PixelFormat, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::rhi::PixelFormat val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};
