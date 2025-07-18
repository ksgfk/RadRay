#include "vulkan_image.h"

#include "vulkan_device.h"

namespace radray::render::vulkan {

ImageVulkanDescriptor ImageVulkanDescriptor::FromRaw(const TextureCreateDescriptor& desc) noexcept {
    ImageVulkanDescriptor result;
    result.Name = desc.Name;
    result.Dim = desc.Dim;
    result.Width = desc.Width;
    result.Height = desc.Height;
    result.DepthOrArraySize = desc.DepthOrArraySize;
    result.MipLevels = desc.MipLevels;
    result.SampleCount = desc.SampleCount;
    result.Format = desc.Format;
    result.Usage = desc.Usage;
    result.Hints = desc.Hints;
    return result;
}

static void DestroyImageVulkan(ImageVulkan& img) noexcept {
    if (img._image != VK_NULL_HANDLE) {
        if (img._allocation == VK_NULL_HANDLE) {
            img._device->CallVk(&FTbVk::vkDestroyImage, img._image, img._device->GetAllocationCallbacks());
            img._image = VK_NULL_HANDLE;
        } else {
            vmaDestroyImage(img._device->_alloc, img._image, img._allocation);
            img._image = VK_NULL_HANDLE;
            img._allocation = VK_NULL_HANDLE;
        }
    }
}

ImageVulkan::~ImageVulkan() noexcept {
    DestroyImageVulkan(*this);
}

bool ImageVulkan::IsValid() const noexcept {
    return _image != VK_NULL_HANDLE;
}

void ImageVulkan::Destroy() noexcept {
    DestroyImageVulkan(*this);
}

ResourceType ImageVulkan::GetType() const noexcept {
    RADRAY_UNIMPLEMENTED();
    return ResourceType::UNKNOWN;
}

ResourceStates ImageVulkan::GetInitState() const noexcept {
    RADRAY_UNIMPLEMENTED();
    return ResourceState::Common;
}

void ImageVulkan::DangerousDestroy() noexcept {
    _allocation = VK_NULL_HANDLE;
    _image = VK_NULL_HANDLE;
}

}  // namespace radray::render::vulkan
