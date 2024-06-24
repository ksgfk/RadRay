#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class D3D12Device;
class D3D12CommandQueue;

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
    ComPtr<IDXGISwapChain3> swapChain;
};

}  // namespace radray::rhi::d3d12
