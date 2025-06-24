#include "vulkan_fence.h"

#include "vulkan_device.h"

namespace radray::render::vulkan {

static void DestroyVulkanFence(FenceVulkan& fence) noexcept {
    if (fence.IsValid()) {
        fence._device->CallVk(&FTbVk::vkDestroyFence, fence._fence, fence._device->GetAllocationCallbacks());
        fence._fence = VK_NULL_HANDLE;
        fence._device = nullptr;
    }
}

FenceVulkan::~FenceVulkan() noexcept {
    DestroyVulkanFence(*this);
}

bool FenceVulkan::IsValid() const noexcept {
    return _device != nullptr && _fence != VK_NULL_HANDLE;
}

void FenceVulkan::Destroy() noexcept {
    DestroyVulkanFence(*this);
}

void FenceVulkan::Wait() noexcept {
    _device->CallVk(&FTbVk::vkWaitForFences, 1, &_fence, VK_TRUE, UINT64_MAX);
}

void FenceVulkan::Reset() noexcept {
    _device->CallVk(&FTbVk::vkResetFences, 1, &_fence);
}

VkResult FenceVulkan::GetStatus() const noexcept {
    return _device->CallVk(&FTbVk::vkGetFenceStatus, _fence);
}

}  // namespace radray::render::vulkan
