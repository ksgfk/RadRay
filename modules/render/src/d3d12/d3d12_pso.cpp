#include "d3d12_pso.h"

namespace radray::render::d3d12 {

bool GraphicsPsoD3D12::IsValid() const noexcept {
    return _pso != nullptr;
}

void GraphicsPsoD3D12::Destroy() noexcept {
    _pso = nullptr;
    _arrayStrides.clear();
    _arrayStrides.shrink_to_fit();
}

}  // namespace radray::render::d3d12
