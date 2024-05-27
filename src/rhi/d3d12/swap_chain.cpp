#include "swap_chain.h"

namespace radray::rhi::d3d12 {

void SwapChain::Present(ICommandQueue* queue) {
    swapChain->Present(isVsync, isVsync ? 0 : DXGI_PRESENT_ALLOW_TEARING);
    backBufferIndex = swapChain->GetCurrentBackBufferIndex();
}

}  // namespace radray::rhi::d3d12
