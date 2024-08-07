#include "d3d12_swapchain.h"

#include "d3d12_device.h"

namespace radray::rhi::d3d12 {

SwapChain::SwapChain(Device* device, const RadraySwapChainDescriptor& desc)
    : presentFlags(0) {
    DXGI_SWAP_CHAIN_DESC1 chain{
        .Width = desc.Width,
        .Height = desc.Height,
        .Format = EnumConvert(desc.Format),
        .Stereo = FALSE,
        .SampleDesc = {.Count = 1, .Quality = 0},
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = desc.BackBufferCount,
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED,
        .Flags = 0};
    BOOL allowTearing = FALSE;
    RADRAY_DX_FTHROW(device->dxgiFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing)));
    chain.Flags |= allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    presentFlags |= (!desc.EnableSync && allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    ID3D12CommandQueue* queue = reinterpret_cast<ID3D12CommandQueue*>(desc.PresentQueue.Native);
    HWND hwnd = reinterpret_cast<HWND>(desc.NativeWindow);
    RADRAY_DX_FTHROW(device->dxgiFactory->CreateSwapChainForHwnd(queue, hwnd, &chain, nullptr, nullptr, swapchain.GetAddressOf()));
    RADRAY_DX_FTHROW(device->dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER)); // 阻止全屏
}

}  // namespace radray::rhi::d3d12
