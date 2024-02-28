#pragma once

#include <radray/d3d12/resource.h>

namespace radray::d3d12 {

class Device;
class CommandQueue;

class SwapChainRenderTarget : public Resource {
public:
    SwapChainRenderTarget(Device* device, ComPtr<ID3D12Resource>&& rt) noexcept;
    ~SwapChainRenderTarget() noexcept override = default;
    SwapChainRenderTarget(SwapChainRenderTarget&&) noexcept = default;
    SwapChainRenderTarget(const SwapChainRenderTarget&) noexcept = delete;
    SwapChainRenderTarget& operator=(SwapChainRenderTarget&&) noexcept = default;
    SwapChainRenderTarget& operator=(const SwapChainRenderTarget&) noexcept = delete;

    ID3D12Resource* GetResource() const noexcept override;
    D3D12_RESOURCE_STATES GetInitState() const noexcept override;

    ComPtr<ID3D12Resource> rt;
};

class SwapChain {
public:
    SwapChain(
        Device* device,
        CommandQueue* queue,
        HWND windowHandle,
        uint32 frameCount,
        uint32 width,
        uint32 height,
        bool vsync);

    void Present();

    ComPtr<IDXGISwapChain3> swapChain;
    std::vector<SwapChainRenderTarget> renderTargets;
    uint32 backBufferIndex;
    bool isVsync;
};

}  // namespace radray::d3d12
