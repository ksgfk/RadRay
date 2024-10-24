#pragma once

#include <optional>
#include <variant>

#include <radray/types.h>
#include <radray/render/common.h>

namespace radray::render {

struct D3D12DeviceDescriptor {
    std::optional<uint32_t> AdapterIndex;
    bool IsEnableDebugLayer;
    bool IsEnableGpuBasedValid;
};

struct MetalDeviceDescriptor {
    std::optional<uint32_t> DeviceIndex;
};

struct VulkanDeviceDescriptor {
};

using DeviceDescriptor = std::variant<D3D12DeviceDescriptor, MetalDeviceDescriptor, VulkanDeviceDescriptor>;

class Device : public radray::enable_shared_from_this<Device> {
public:
    virtual ~Device() noexcept = default;

    virtual Backend GetBackend() noexcept = 0;
};

std::optional<radray::shared_ptr<Device>> CreateDevice(const DeviceDescriptor& desc);

}  // namespace radray::render
