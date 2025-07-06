#pragma once

#include <radray/render/swap_chain.h>

#include "vulkan_helper.h"
#include "vulkan_image.h"

namespace radray::render::vulkan {

class SwapChainVulkan : public SwapChain {
public:
    SwapChainVulkan(
        DeviceVulkan* device,
        VkSurfaceKHR surface,
        VkSwapchainKHR swapchain,
        vector<shared_ptr<ImageVulkan>> colors) noexcept
        : _device(device),
          _surface(surface),
          _swapchain(swapchain),
          _colors(std::move(colors)) {}

    ~SwapChainVulkan() noexcept;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    Nullable<Texture> AcquireNextRenderTarget() noexcept override;

    Texture* GetCurrentRenderTarget() noexcept override;

    void Present() noexcept override;

public:
    DeviceVulkan* _device;
    VkSurfaceKHR _surface;
    VkSwapchainKHR _swapchain;
    vector<shared_ptr<ImageVulkan>> _colors;
};

}  // namespace radray::render::vulkan
