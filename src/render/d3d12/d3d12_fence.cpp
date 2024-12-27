#include "d3d12_fence.h"

namespace radray::render::d3d12 {

void FenceD3D12::Destroy() noexcept {
    _fence = nullptr;
    _event.Destroy();
}

}  // namespace radray::render::d3d12
