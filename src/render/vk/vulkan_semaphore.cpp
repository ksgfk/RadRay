#include "vulkan_semaphore.h"

#include "vulkan_device.h"

namespace radray::render::vulkan {

static void DestroySemaphoreVulkan(SemaphoreVulkan& s) noexcept {
    if (s._semaphore != VK_NULL_HANDLE) {
        s._device->CallVk(&FTbVk::vkDestroySemaphore, s._semaphore, s._device->GetAllocationCallbacks());
        s._semaphore = VK_NULL_HANDLE;
    }
}

SemaphoreVulkan::SemaphoreVulkan(
    DeviceVulkan* device,
    VkSemaphore semaphore) noexcept
    : _device(device),
      _semaphore(semaphore) {}

SemaphoreVulkan::~SemaphoreVulkan() noexcept {
    DestroySemaphoreVulkan(*this);
}

bool SemaphoreVulkan::IsValid() const noexcept {
    return _semaphore != VK_NULL_HANDLE;
}

void SemaphoreVulkan::Destroy() noexcept {
    DestroySemaphoreVulkan(*this);
}

}  // namespace radray::render::vulkan
