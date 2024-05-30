#pragma once

#include <optional>

namespace radray::rhi {

enum class CommandListType {
    Graphics,
    Compute
};

enum class BufferType {
    Default,
    Upload,
    Readback
};

struct DeviceCreateInfoD3D12 {
    std::optional<uint32_t> AdapterIndex;
    bool IsEnableDebugLayer;
};

struct SwapChainCreateInfo {
};

struct CommandQueueCreateInfo {
    CommandListType Type;
};

struct FenceCreateInfo {
};

struct BufferCreateInfo {
};

struct TextureCreateInfo {
};

struct ShaderCreateInfo {
};

}  // namespace radray::rhi
