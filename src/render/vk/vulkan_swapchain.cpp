#include "vulkan_swapchain.h"

#include "vulkan_device.h"

namespace radray::render::vulkan {

static void DestroySwapChainVulkan(SwapChainVulkan& s) noexcept {
    for (auto& i : s._frames) {
        i._color->DangerousDestroy();
    }
    s._frames.clear();
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
    shared_ptr<SemaphoreVulkan> acquireSemaphore;
    if (_semaphorePool.empty()) {
        acquireSemaphore = _device->CreateSemaphoreVk().Unwrap();
    } else {
        acquireSemaphore = std::move(_semaphorePool.back());
        _semaphorePool.pop_back();
    }
    // 确保在渲染命令开始执行前，SwapChain中的图像已经被成功 acquire，并且可供渲染使用
    uint32_t imageIndex;
    if (auto vr = _device->CallVk(&FTbVk::vkAcquireNextImageKHR, _swapchain, UINT64_MAX, acquireSemaphore->_semaphore, VK_NULL_HANDLE, &imageIndex);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkAcquireNextImageKHR failed: {}", vr);
        _semaphorePool.emplace_back(std::move(acquireSemaphore));
        return nullptr;
    }
    auto& frame = _frames[imageIndex];
    if (frame._submitFence.HasValue()) {
        frame._submitFence->Wait();
        frame._submitFence->Reset();
    }
    if (frame._acquireSemaphore.HasValue()) {
        _semaphorePool.emplace_back(frame._acquireSemaphore.Release());
        frame._acquireSemaphore = nullptr;
    }
    frame._acquireSemaphore = std::move(acquireSemaphore);
    _currentFrameIndex = imageIndex;
    return frame._color.get();
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
