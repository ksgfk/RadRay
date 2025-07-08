#pragma once

#include <radray/render/swap_chain.h>

#include "vulkan_helper.h"
#include "vulkan_image.h"

namespace radray::render::vulkan {

class SwapChainFrame {
public:
    unique_ptr<ImageVulkan> _color;
    unique_ptr<SemaphoreVulkan> _acquireSemaphore;
    unique_ptr<SemaphoreVulkan> _releaseSemaphore;
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
    vector<shared_ptr<ImageVulkan>> _colors;
    vector<SwapChainFrame> _frames;
    uint32_t _currentFrameIndex{0};
};

}  // namespace radray::render::vulkan
