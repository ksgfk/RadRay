#pragma once

#include "vulkan_helper.h"
#include <radray/render/resource.h>

namespace radray::render::vulkan {

class ImageVulkanDescriptor {
public:
    string Name;
    TextureDimension Dim;
    uint32_t Width;
    uint32_t Height;
    uint32_t DepthOrArraySize;
    uint32_t MipLevels;
    uint32_t SampleCount;
    TextureFormat Format;
    TextureUses Usage;
    ResourceStates InitState;
    ClearValue Clear;
    ResourceHints Hints;

    VkFormat _rawFormat;

    static ImageVulkanDescriptor FromRaw(const TextureCreateDescriptor& desc) noexcept;
};

class ImageVulkan : public Texture {
public:
    ImageVulkan(
        DeviceVulkan* device,
        VkImage image,
        VmaAllocation allocation,
        const VmaAllocationInfo& info,
        const ImageVulkanDescriptor& desc) noexcept
        : _device(device),
          _image(image),
          _allocation(allocation),
          _info(info),
          _desc(desc) {}

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
    ImageVulkanDescriptor _desc;
};

}  // namespace radray::render::vulkan
