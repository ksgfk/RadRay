#pragma once

#include <radray/rhi/swap_chain.h>

#include "device.h"

namespace radray::rhi::d3d12 {

class SwapChain : public ISwapChain {
public:
    ~SwapChain() noexcept override = default;

    void Present(ICommandQueue* queue) override;

public:
    Device* device;
    DXGI_SWAP_CHAIN_DESC1 createDesc;
    ComPtr<IDXGISwapChain3> swapChain;
    // std::vector<SwapChainRenderTarget> renderTargets;
    uint32_t backBufferIndex;
    bool isVsync;
};

}  // namespace radray::rhi::d3d12
