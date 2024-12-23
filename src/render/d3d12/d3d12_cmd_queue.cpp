#include "d3d12_cmd_queue.h"

#include "d3d12_device.h"

namespace radray::render::d3d12 {

void CmdQueueD3D12::Destroy() noexcept {
    _queue = nullptr;
}

}  // namespace radray::render::d3d12
