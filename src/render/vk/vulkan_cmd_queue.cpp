#include "vulkan_cmd_queue.h"

namespace radray::render::vulkan {

QueueVulkan::~QueueVulkan() noexcept {
}

bool QueueVulkan::IsValid() const noexcept {
    return false;
}

void QueueVulkan::Destroy() noexcept {}

Nullable<shared_ptr<CommandBuffer>> QueueVulkan::CreateCommandBuffer() noexcept {
    return nullptr;
}

void QueueVulkan::Submit(std::span<CommandBuffer*> buffers, Nullable<Fence> singalFence) noexcept {}

void QueueVulkan::Wait() noexcept {}

void QueueVulkan::WaitFences(std::span<Fence*> fences) noexcept {}

}  // namespace radray::render::vulkan
