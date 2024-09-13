#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Device;
class Texture;

class SwapChain {
public:
    SwapChain(
        Device* device,
        ID3D12CommandQueue* queue,
        HWND hwnd,
        const DXGI_SWAP_CHAIN_DESC1& desc,
        bool enableSync);

public:
    radray::vector<radray::unique_ptr<Texture>> colors;
    ComPtr<IDXGISwapChain3> swapchain;
    UINT presentFlags;
};

}  // namespace radray::rhi::d3d12
