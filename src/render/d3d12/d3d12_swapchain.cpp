#include "d3d12_swapchain.h"

#include "d3d12_texture.h"

namespace radray::render::d3d12 {

bool SwapChainD3D12::IsValid() const noexcept { return _swapchain != nullptr; }

void SwapChainD3D12::Destroy() noexcept {
    _swapchain = nullptr;
}

Texture* SwapChainD3D12::AcquireNextRenderTarget() noexcept {
    UINT curr = _swapchain->GetCurrentBackBufferIndex();
    if (curr >= _colors.size()) {
        return nullptr;
    }
    return _colors[curr].get();
}

Texture* SwapChainD3D12::GetCurrentRenderTarget() noexcept {
    UINT curr = _swapchain->GetCurrentBackBufferIndex();
    if (curr >= _colors.size()) {
        return nullptr;
    }
    return _colors[curr].get();
}

void SwapChainD3D12::Present() noexcept {
    _swapchain->Present(0, _presentFlags);
}

}  // namespace radray::render::d3d12
