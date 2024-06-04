#pragma once

#include <radray/rhi/swap_chain.h>

#include "device.h"
#include "texture.h"

namespace radray::rhi::d3d12 {

class CommandQueue;

// class SwapChainRenderTarget : public Texture {
// public:
//     SwapChainRenderTarget(
//         std::shared_ptr<Device> device,
//         ComPtr<ID3D12Resource> rt,
//         uint32_t width,
//         uint32_t height,
//         DXGI_FORMAT format);
//     ~SwapChainRenderTarget() noexcept override = default;

//     D3D12_RENDER_TARGET_VIEW_DESC GetRtvDesc() const noexcept;
// };

class SwapChain : public ISwapChain {
public:
    SwapChain(
        std::shared_ptr<Device> device,
        CommandQueue* queue,
        HWND hwnd,
        uint32_t frameCount,
        uint32_t width,
        uint32_t height,
        bool vsync);
    ~SwapChain() noexcept override = default;

    void Present(ICommandQueue* queue) override;

public:
    DXGI_SWAP_CHAIN_DESC1 createDesc;
    ComPtr<IDXGISwapChain3> swapChain;
    // std::vector<std::unique_ptr<SwapChainRenderTarget>> renderTargets;
    uint32_t backBufferIndex;
    bool isVsync;
};

}  // namespace radray::rhi::d3d12
