#include "metal_cmd_queue.h"

#include "metal_cmd_pool.h"

namespace radray::render::metal {

void CmdQueueMetal::Destroy() noexcept {
    _queue.reset();
}

std::optional<std::shared_ptr<CommandPool>> CmdQueueMetal::CreateCommandPool() noexcept {
    return AutoRelease([this]() {
        auto p = std::make_shared<CmdPoolMetal>(_queue.get());
        return p;
    });
}

}  // namespace radray::render::metal
