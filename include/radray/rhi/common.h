#pragma once

#include <optional>

namespace radray::rhi {

struct DeviceCreateInfoD3D12 {
    std::optional<uint32_t> AdapterIndex;
    bool IsEnableDebugLayer;
};

struct SwapChainCreateInfo {
};

struct CommandQueueCreateInfo {
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
