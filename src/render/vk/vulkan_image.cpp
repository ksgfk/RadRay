#include "vulkan_image.h"

#include "vulkan_device.h"

namespace radray::render::vulkan {

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
    return ResourceType::UNKNOWN;
}

ResourceStates ImageVulkan::GetInitState() const noexcept {
    return ResourceState::Common;
}

}  // namespace radray::render::vulkan
