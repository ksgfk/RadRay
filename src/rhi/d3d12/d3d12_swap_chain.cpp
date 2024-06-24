#include "d3d12_swap_chain.h"

#include "d3d12_device.h"
#include "d3d12_command_queue.h"

namespace radray::rhi::d3d12 {

D3D12SwapChain::D3D12SwapChain(
    D3D12Device* device,
    D3D12CommandQueue* queue,
    HWND windowHandle,
    uint32_t width,
    uint32_t height,
    uint32_t backBufferCount,
    bool vsync) {
    DXGI_SWAP_CHAIN_DESC1 desc{
        .Width = width,
        .Height = height,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = {.Count = 1},
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = backBufferCount,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .Flags = 0};
    {
        ComPtr<IDXGISwapChain1> tempSc;
        RADRAY_DX_CHECK(device->dxgiFactory->CreateSwapChainForHwnd(
            queue->queue.Get(),
            windowHandle,
            &desc,
            nullptr,
            nullptr,
            tempSc.GetAddressOf()));
        tempSc.As(&swapChain);
    }
}

}  // namespace radray::rhi::d3d12
