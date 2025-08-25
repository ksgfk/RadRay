#include "d3d12_fence.h"

namespace radray::render::d3d12 {

static void DestroyFence(FenceD3D12& f) noexcept {
    if (f.IsValid()) {
        f._fence = nullptr;
        f._event.Destroy();
    }
}

FenceD3D12::~FenceD3D12() noexcept {
    DestroyFence(*this);
}

void FenceD3D12::Destroy() noexcept {
    DestroyFence(*this);
}

void FenceD3D12::Wait() noexcept {
    UINT64 completeValue = _fence->GetCompletedValue();
    if (completeValue < _fenceValue) {
        RADRAY_DX_CHECK(_fence->SetEventOnCompletion(_fenceValue, _event.Get()));
        WaitForSingleObject(_event.Get(), INFINITE);
    }
}

}  // namespace radray::render::d3d12
