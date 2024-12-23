#include "d3d12_cmd_allocator.h"

#include "d3d12_device.h"

namespace radray::render::d3d12 {

void CmdAllocatorD3D12::Destroy() noexcept {
    _cmdAlloc = nullptr;
}

}  // namespace radray::render::d3d12
