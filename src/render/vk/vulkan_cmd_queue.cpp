#include "vulkan_cmd_queue.h"

namespace radray::render::vulkan {

static void DestroyQueueVulkan(QueueVulkan& q) noexcept {
    if (q.IsValid()) {
        q._device = nullptr;
        q._queue = VK_NULL_HANDLE;
    }
}

QueueVulkan::~QueueVulkan() noexcept {
    DestroyQueueVulkan(*this);
}

bool QueueVulkan::IsValid() const noexcept {
    return _device != nullptr && _queue != VK_NULL_HANDLE;
}

void QueueVulkan::Destroy() noexcept {
    DestroyQueueVulkan(*this);
}

Nullable<shared_ptr<CommandBuffer>> QueueVulkan::CreateCommandBuffer() noexcept {
    return nullptr;
}

void QueueVulkan::Submit(std::span<CommandBuffer*> buffers, Nullable<Fence> singalFence) noexcept {}

void QueueVulkan::Wait() noexcept {}

void QueueVulkan::WaitFences(std::span<Fence*> fences) noexcept {}

}  // namespace radray::render::vulkan
