#include "d3d12_cmd_queue.h"

namespace radray::render::d3d12 {

void CmdQueueD3D12::Destroy() noexcept {
    _queue = nullptr;
}

std::optional<std::shared_ptr<CommandPool>> CmdQueueD3D12::CreateCommandPool(std::string_view debugName) noexcept {
    return std::nullopt;
}

}  // namespace radray::render::d3d12
