#include "d3d12_swapchain.h"

#include "d3d12_device.h"
#include "d3d12_texture.h"

namespace radray::rhi::d3d12 {

SwapChain::SwapChain(
    Device* device,
    ID3D12CommandQueue* queue,
    HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1& desc,
    bool enableSync)
    : presentFlags(0) {
    DXGI_SWAP_CHAIN_DESC1 chain = desc;
    BOOL allowTearing = FALSE;
    RADRAY_DX_FTHROW(device->dxgiFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing)));
    chain.Flags |= allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    presentFlags |= (!enableSync && allowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> temp;
    RADRAY_DX_FTHROW(device->dxgiFactory->CreateSwapChainForHwnd(queue, hwnd, &chain, nullptr, nullptr, temp.GetAddressOf()));
    RADRAY_DX_FTHROW(device->dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));  // 阻止全屏
    RADRAY_DX_FTHROW(temp->QueryInterface(IID_PPV_ARGS(swapchain.GetAddressOf())));
    colors.reserve(desc.BufferCount);
    for (size_t i = 0; i < desc.BufferCount; i++) {
        ComPtr<ID3D12Resource> color;
        RADRAY_DX_FTHROW(swapchain->GetBuffer(i, IID_PPV_ARGS(color.GetAddressOf())));
        auto rtTex = std::make_unique<Texture>(std::move(color), D3D12_RESOURCE_STATE_PRESENT);
        colors.emplace_back(std::move(rtTex));
    }
}

}  // namespace radray::rhi::d3d12
