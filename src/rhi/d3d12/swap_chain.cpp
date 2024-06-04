#include "swap_chain.h"

#include "command_queue.h"

namespace radray::rhi::d3d12 {

// SwapChainRenderTarget::SwapChainRenderTarget(
//     std::shared_ptr<Device> device,
//     ComPtr<ID3D12Resource> rt,
//     uint32_t width,
//     uint32_t height,
//     DXGI_FORMAT format)
//     : Texture(
//           std::move(device),
//           std::move(rt),
//           nullptr,
//           D3D12_RESOURCE_STATE_PRESENT) {}

// D3D12_RENDER_TARGET_VIEW_DESC SwapChainRenderTarget::GetRtvDesc() const noexcept {
//     D3D12_RENDER_TARGET_VIEW_DESC desc{
//         // .Format = format,
//         .ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D};
//     desc.Texture2D = {
//         .MipSlice = 0,
//         .PlaneSlice = 0};
//     return desc;
// }

SwapChain::SwapChain(
    std::shared_ptr<Device> device,
    CommandQueue* queue,
    HWND hwnd,
    uint32_t frameCount,
    uint32_t width,
    uint32_t height,
    bool vsync)
    : ISwapChain(std::move(device)),
      isVsync(vsync) {
    BOOL canTearing = true;
    RADRAY_DX_CHECK(device->dxgiFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &canTearing, sizeof(decltype(canTearing))));
    RADRAY_INFO_LOG("DXGI_FEATURE_PRESENT_ALLOW_TEARING: {}", (bool)canTearing);
    if (!canTearing) {
        isVsync = true;
    }
    if (!vsync && isVsync) {
        RADRAY_WARN_LOG("DXGI check tearing feature doesn't support. vsync-off cannot work");
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
        RADRAY_DX_CHECK(device->dxgiFactory->CreateSwapChainForHwnd(
            queue->queue.Get(),
            hwnd,
            &desc,
            nullptr,
            nullptr,
            tempSc.GetAddressOf()));
        tempSc.As(&swapChain);
    }
    // renderTargets.reserve(desc.BufferCount);
}

void SwapChain::Present(ICommandQueue* queue) {
    swapChain->Present(isVsync, isVsync ? 0 : DXGI_PRESENT_ALLOW_TEARING);
    backBufferIndex = swapChain->GetCurrentBackBufferIndex();
}

}  // namespace radray::rhi::d3d12
