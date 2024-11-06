#include "metal_cmd_queue.h"

#include "metal_device.h"
#include "metal_cmd_pool.h"

namespace radray::render::metal {

void CmdQueueMetal::Destroy() noexcept {
    _queue.reset();
}

std::optional<radray::shared_ptr<CommandPool>> CmdQueueMetal::CreateCommandPool() noexcept {
    return AutoRelease([this]() {
        auto p = radray::make_shared<CmdPoolMetal>(std::static_pointer_cast<DeviceMetal>(_device->shared_from_this()), _queue.get());
        return p;
    });
}

}  // namespace radray::render::metal
