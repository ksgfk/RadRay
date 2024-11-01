#include "metal_cmd_queue.h"

namespace radray::render::metal {

void CmdQueueMetal::Destroy() noexcept {
    _queue.reset();
}

std::optional<std::shared_ptr<CommandPool>> CmdQueueMetal::CreateCommandPool() noexcept {
    return std::nullopt;
}

}  // namespace radray::render::metal
