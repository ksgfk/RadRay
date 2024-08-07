#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Device;

class SwapChain {
public:
    SwapChain(Device* device, const RadraySwapChainDescriptor& desc);

public:
    ComPtr<IDXGISwapChain1> swapchain;
    UINT presentFlags;
};

}  // namespace radray::rhi::d3d12
