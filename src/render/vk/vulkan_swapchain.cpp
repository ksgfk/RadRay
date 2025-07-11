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
    } else {
        frame._submitFence = std::static_pointer_cast<FenceVulkan>(_device->CreateFence().Unwrap());
    }
    frame._submitFence->Reset();
    if (frame._acquireSemaphore.HasValue()) {
        _semaphorePool.emplace_back(frame._acquireSemaphore.Release());
        frame._acquireSemaphore = nullptr;
    }
    frame._acquireSemaphore = std::move(acquireSemaphore);
    _currentFrameIndex = imageIndex;
    return frame._color.get();
}

Texture* SwapChainVulkan::GetCurrentRenderTarget() noexcept {
    return _frames[_currentFrameIndex]._color.get();
}

void SwapChainVulkan::Present() noexcept {
    auto& frame = _frames[_currentFrameIndex];
    if (!frame._releaseSemaphore.HasValue()) {
        frame._releaseSemaphore = _device->CreateSemaphoreVk().Unwrap();
    }
    VkPipelineStageFlags waitStage{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext = nullptr;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &frame._acquireSemaphore->_semaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 0;
    submitInfo.pCommandBuffers = nullptr;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &frame._releaseSemaphore->_semaphore;
    if (auto vr = _device->CallVk(&FTbVk::vkQueueSubmit, _queue->_queue, 1, &submitInfo, frame._submitFence->_fence);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkQueueSubmit failed: {}", vr);
        return;
    }
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &frame._releaseSemaphore->_semaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.pImageIndices = &_currentFrameIndex;
    presentInfo.pResults = nullptr;
    if (auto vr = _device->CallVk(&FTbVk::vkQueuePresentKHR, _queue->_queue, &presentInfo);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkQueuePresentKHR failed: {}", vr);
        return;
    }
}

}  // namespace radray::render::vulkan
