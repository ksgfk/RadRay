#include "d3d12_swapchain.h"

namespace radray::render::d3d12 {

bool SwapChainD3D12::IsValid() const noexcept { return _swapchain != nullptr; }

void SwapChainD3D12::Destroy() noexcept {
    _swapchain = nullptr;
}

}  // namespace radray::render::d3d12
