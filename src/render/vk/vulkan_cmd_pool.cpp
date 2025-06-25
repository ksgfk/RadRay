#include "vulkan_cmd_pool.h"

#include "vulkan_device.h"

namespace radray::render::vulkan {

static void DestroyCommandPoolVulkan(CommandPoolVulkan& pool) noexcept {
    if (pool.IsValid()) {
        pool._device->CallVk(&FTbVk::vkDestroyCommandPool, pool._pool, pool._device->GetAllocationCallbacks());
        pool._pool = VK_NULL_HANDLE;
    }
}

CommandPoolVulkan::~CommandPoolVulkan() noexcept {
    DestroyCommandPoolVulkan(*this);
}

bool CommandPoolVulkan::IsValid() const noexcept {
    return _pool != VK_NULL_HANDLE;
}

void CommandPoolVulkan::Destroy() noexcept {
    DestroyCommandPoolVulkan(*this);
}

}  // namespace radray::render::vulkan
