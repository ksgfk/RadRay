#pragma once

#include <radray/render/swap_chain.h>

#include "vulkan_helper.h"
#include "vulkan_image.h"
#include "vulkan_semaphore.h"
#include "vulkan_fence.h"

namespace radray::render::vulkan {

class SwapChainFrame {
public:
    shared_ptr<ImageVulkan> _color;
    Nullable<shared_ptr<SemaphoreVulkan>> _acquireSemaphore;
    Nullable<shared_ptr<SemaphoreVulkan>> _releaseSemaphore;
    Nullable<shared_ptr<FenceVulkan>> _submitFence;
};

class SwapChainVulkan : public SwapChain {
public:
    SwapChainVulkan(DeviceVulkan* device, QueueVulkan* queue) noexcept
        : _device(device), _queue(queue) {}

    ~SwapChainVulkan() noexcept;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    Nullable<Texture> AcquireNextRenderTarget() noexcept override;

    Texture* GetCurrentRenderTarget() noexcept override;

    void Present() noexcept override;

public:
    DeviceVulkan* _device;
    QueueVulkan* _queue;
    VkSurfaceKHR _surface{VK_NULL_HANDLE};
    VkSwapchainKHR _swapchain{VK_NULL_HANDLE};
    vector<SwapChainFrame> _frames;
    vector<shared_ptr<SemaphoreVulkan>> _semaphorePool;
    uint32_t _currentFrameIndex{0};
};

}  // namespace radray::render::vulkan
