#pragma once

#include "vulkan_helper.h"

namespace radray::render::vulkan {

class CommandPoolVulkan : public RenderBase {
public:
    CommandPoolVulkan(DeviceVulkan* device, VkCommandPool pool) noexcept
        : _device(device), _pool(pool) {}

    ~CommandPoolVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    DeviceVulkan* _device;
    VkCommandPool _pool;
};

}  // namespace radray::render::vulkan
