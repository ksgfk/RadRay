#include "vulkan_swapchain.h"

#include "vulkan_device.h"

namespace radray::render::vulkan {

static void DestroySwapChainVulkan(SwapChainVulkan& s) noexcept {
    for (auto& i : s._colors) {
        i->DangerousDestroy();
    }
    s._colors.clear();
    if (s._swapchain != VK_NULL_HANDLE) {
        s._device->CallVk(&FTbVk::vkDestroySwapchainKHR, s._swapchain, s._device->GetAllocationCallbacks());
        s._swapchain = VK_NULL_HANDLE;
    }
    if (s._surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(s._device->_instance, s._surface, s._device->GetAllocationCallbacks());
        s._surface = VK_NULL_HANDLE;
    }
}

SwapChainVulkan::~SwapChainVulkan() noexcept {
    DestroySwapChainVulkan(*this);
}

bool SwapChainVulkan::IsValid() const noexcept {
    return _swapchain != VK_NULL_HANDLE && _surface != VK_NULL_HANDLE;
}

void SwapChainVulkan::Destroy() noexcept {
    DestroySwapChainVulkan(*this);
}

Nullable<Texture> SwapChainVulkan::AcquireNextRenderTarget() noexcept {
    RADRAY_UNIMPLEMENTED();
    return nullptr;
}

Texture* SwapChainVulkan::GetCurrentRenderTarget() noexcept {
    RADRAY_UNIMPLEMENTED();
    return nullptr;
}

void SwapChainVulkan::Present() noexcept {
    // VkSubmitInfo submitInfo{};
    // submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    // submitInfo.pNext = nullptr;
    // submitInfo.waitSemaphoreCount = 1;
    RADRAY_UNIMPLEMENTED();
}

}  // namespace radray::render::vulkan
