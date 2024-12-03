#pragma once

#include <radray/render/swap_chain.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class SwapChainD3D12 : public SwapChain {
public:
    SwapChainD3D12(ComPtr<IDXGISwapChain3> swapchain, UINT presentFlags) noexcept
        : _swapchain(std::move(swapchain)),
          _presentFlags(presentFlags) {}
    ~SwapChainD3D12() noexcept override = default;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;

public:
    ComPtr<IDXGISwapChain3> _swapchain;
    UINT _presentFlags;
};

}  // namespace radray::render::d3d12
