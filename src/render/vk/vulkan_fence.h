#pragma once

#include "vulkan_helper.h"
#include <radray/render/fence.h>

namespace radray::render::vulkan {

class DeviceVulkan;

class FenceVulkan : public Fence {
public:
    FenceVulkan(DeviceVulkan* device, VkFence fence) noexcept
        : _device(device), _fence(fence) {};

    ~FenceVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void Wait() noexcept override;

    void Reset() noexcept;

    /** @return VK_SUCCESS | VK_NOT_READY */
    VkResult GetStatus() const noexcept;

public:
    DeviceVulkan* _device;
    VkFence _fence;
};

}  // namespace radray::render::vulkan
