#include <radray/d3d12/swap_chain.h>

#include <radray/d3d12/device.h>
#include <radray/d3d12/command_queue.h>

namespace radray::d3d12 {

SwapChainRenderTarget::SwapChainRenderTarget(Device* device, ComPtr<ID3D12Resource>&& rt) noexcept : Resource(device), rt(std::move(rt)) {}

ID3D12Resource* SwapChainRenderTarget::GetResource() const noexcept {
    return rt.Get();
}

D3D12_RESOURCE_STATES SwapChainRenderTarget::GetInitState() const noexcept {
    return D3D12_RESOURCE_STATE_PRESENT;
}

D3D12_RENDER_TARGET_VIEW_DESC SwapChainRenderTarget::GetRtvDesc() const noexcept {
    D3D12_RENDER_TARGET_VIEW_DESC desc{
        .Format = format,
        .ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D};
    desc.Texture2D = {
        .MipSlice = 0,
        .PlaneSlice = 0};
    return desc;
}

SwapChain::SwapChain(
    Device* device,
    CommandQueue* queue,
    HWND windowHandle,
    uint32 frameCount,
    uint32 width,
    uint32 height,
    bool vsync)
    : backBufferIndex(0),
      isVsync(vsync) {
    BOOL canTearing = true;
    ThrowIfFailed(device->dxgiFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &canTearing, sizeof(decltype(canTearing))));
    RADRAY_LOG_INFO("DXGI_FEATURE_PRESENT_ALLOW_TEARING: {}", (bool)canTearing);
    if (!canTearing) {
        isVsync = true;
    }
    if (!vsync && isVsync) {
        RADRAY_LOG_WARN("DXGI check tearing feature doesn't support. vsync-off cannot work");
    }
    DXGI_SWAP_CHAIN_DESC1 desc{
        .Width = width,
        .Height = height,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = {.Count = 1},
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = frameCount,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .Flags = canTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u};
    {
        ComPtr<IDXGISwapChain1> tempSc;
        ThrowIfFailed(device->dxgiFactory->CreateSwapChainForHwnd(
            queue->queue.Get(),
            windowHandle,
            &desc,
            nullptr,
            nullptr,
            tempSc.GetAddressOf()));
        tempSc.As(&swapChain);
    }
    renderTargets.reserve(desc.BufferCount);
    for (uint32_t n = 0; n < desc.BufferCount; n++) {
        ComPtr<ID3D12Resource> rt;
        ThrowIfFailed(swapChain->GetBuffer(n, IID_PPV_ARGS(rt.GetAddressOf())));
        auto&& rtRes = renderTargets.emplace_back(SwapChainRenderTarget{device, std::move(rt)});
        rtRes.format = desc.Format;
    }
    backBufferIndex = swapChain->GetCurrentBackBufferIndex();
}

void SwapChain::Present() {
    swapChain->Present(isVsync, isVsync ? 0 : DXGI_PRESENT_ALLOW_TEARING);
    backBufferIndex = swapChain->GetCurrentBackBufferIndex();
}

}  // namespace radray::d3d12
