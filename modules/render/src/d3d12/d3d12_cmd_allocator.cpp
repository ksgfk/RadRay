#include "d3d12_cmd_allocator.h"

#include "d3d12_device.h"

namespace radray::render::d3d12 {

void CmdAllocatorD3D12::Destroy() noexcept {
    _cmdAlloc = nullptr;
}

void CmdAllocatorD3D12::Reset() noexcept {
    RADRAY_DX_CHECK(_cmdAlloc->Reset());
}

}  // namespace radray::render::d3d12
