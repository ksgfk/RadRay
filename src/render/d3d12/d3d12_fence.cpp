#include "d3d12_fence.h"

namespace radray::render::d3d12 {

void FenceD3D12::Destroy() noexcept {
    _fence = nullptr;
    _event.Destroy();
}

void FenceD3D12::Wait() noexcept {
    UINT64 completeValue = _fence->GetCompletedValue();
    if (completeValue < _fenceValue) {
        RADRAY_DX_CHECK(_fence->SetEventOnCompletion(_fenceValue, _event.Get()));
        WaitForSingleObject(_event.Get(), INFINITE);
    }
}

}  // namespace radray::render::d3d12
