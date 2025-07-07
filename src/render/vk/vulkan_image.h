#pragma once

#include "vulkan_helper.h"
#include <radray/render/resource.h>

namespace radray::render::vulkan {

class ImageVulkan : public Texture {
public:
    ImageVulkan(
        DeviceVulkan* device,
        VkImage image,
        VmaAllocation allocation,
        const VmaAllocationInfo& info)
        : _device(device),
          _image(image),
          _allocation(allocation),
          _info(info) {}

    ~ImageVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    ResourceType GetType() const noexcept override;

    ResourceStates GetInitState() const noexcept override;

    void DangerousDestroy() noexcept;

public:
    DeviceVulkan* _device;
    VkImage _image;
    VmaAllocation _allocation;
    VmaAllocationInfo _info;
};

}  // namespace radray::render::vulkan
