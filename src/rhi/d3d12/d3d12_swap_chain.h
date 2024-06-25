#pragma once

#include <vector>

#include "d3d12_helper.h"
#include "d3d12_texture.h"

namespace radray::rhi::d3d12 {

class D3D12Device;
class D3D12CommandQueue;
class D3D12SwapChain;

class D3D12SwapChainRenderTarget : public D3D12Texture {
public:
    D3D12SwapChainRenderTarget(D3D12SwapChain* swapchain, uint32_t index);
    ~D3D12SwapChainRenderTarget() noexcept override = default;
};

class D3D12SwapChain {
public:
    D3D12SwapChain(
        D3D12Device* device,
        D3D12CommandQueue* queue,
        HWND windowHandle,
        uint32_t width,
        uint32_t height,
        uint32_t backBufferCount,
        bool vsync);

public:
    std::vector<std::unique_ptr<D3D12SwapChainRenderTarget>> renderTargets;
    ComPtr<IDXGISwapChain3> swapChain;
};

}  // namespace radray::rhi::d3d12
