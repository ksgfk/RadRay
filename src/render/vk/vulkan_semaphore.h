#pragma once

#include "vulkan_helper.h"

namespace radray::render::vulkan {

class SemaphoreVulkan : public RenderBase {
public:
    SemaphoreVulkan(DeviceVulkan* device, VkSemaphore semaphore) noexcept;

    ~SemaphoreVulkan() noexcept override;

    RenderObjectTags GetTag() const noexcept override { return RenderObjectTag::UNKNOWN; }

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    DeviceVulkan* _device;
    VkSemaphore _semaphore{VK_NULL_HANDLE};
};

}  // namespace radray::render::vulkan
