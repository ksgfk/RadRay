#pragma once

#include "vulkan_helper.h"

namespace radray::render::vulkan {

class CommandPoolVulkan : public RenderBase {
public:
    CommandPoolVulkan(DeviceVulkan* device, VkCommandPool pool) noexcept
        : _device(device), _pool(pool) {}

    ~CommandPoolVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void Reset() const noexcept;

public:
    DeviceVulkan* _device;
    VkCommandPool _pool;
};

}  // namespace radray::render::vulkan
