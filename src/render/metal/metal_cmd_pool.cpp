#include "metal_cmd_pool.h"

#include "metal_cmd_buffer.h"

namespace radray::render::metal {

void CmdPoolMetal::Destroy() noexcept {
    _queue = nullptr;
}

std::optional<std::shared_ptr<CommandBuffer>> CmdPoolMetal::CreateCommandBuffer() noexcept {
    return AutoRelease([this]() {
        MTL::CommandBuffer* buf = _queue->commandBufferWithUnretainedReferences();
        auto b = std::make_shared<CmdBufferMetal>(NS::RetainPtr(buf));
        return b;
    });
}

}  // namespace radray::render::metal
