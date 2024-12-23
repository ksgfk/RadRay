#pragma once

#include <radray/render/swap_chain.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class SwapChainD3D12 : public SwapChain {
public:
    SwapChainD3D12(
        ComPtr<IDXGISwapChain3> swapchain,
        radray::vector<radray::shared_ptr<TextureD3D12>> colors,
        UINT presentFlags) noexcept
        : _swapchain(std::move(swapchain)),
          _colors(std::move(colors)),
          _presentFlags(presentFlags) {}
    ~SwapChainD3D12() noexcept override = default;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;

    Texture* AcquireNextRenderTarget() noexcept override;
    Texture* GetCurrentRenderTarget() noexcept override;
    void Present() noexcept override;

public:
    ComPtr<IDXGISwapChain3> _swapchain;
    radray::vector<radray::shared_ptr<TextureD3D12>> _colors;
    UINT _presentFlags;
};

}  // namespace radray::render::d3d12
