#include "vulkan_cmd_queue.h"

#include "vulkan_fence.h"
#include "vulkan_device.h"
#include "vulkan_cmd_pool.h"
#include "vulkan_cmd_buffer.h"

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
    VkCommandPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        nullptr,
        0,
        _inFamily.Family};
    VkCommandPool pool{VK_NULL_HANDLE};
    if (auto vr = _device->CallVk(&FTbVk::vkCreateCommandPool, _device->_device, &poolInfo, _device->GetAllocationCallbacks(), &pool);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkCreateCommandPool failed: {}", vr);
        return nullptr;
    }
    auto mPool = make_unique<CommandPoolVulkan>(_device, pool);
    VkCommandBufferAllocateInfo bufferInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        nullptr,
        mPool->_pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        1};
    VkCommandBuffer cmdBuf{VK_NULL_HANDLE};
    if (auto vr = _device->CallVk(&FTbVk::vkAllocateCommandBuffers, _device->_device, &bufferInfo, &cmdBuf);
        vr != VK_SUCCESS) {
        RADRAY_ERR_LOG("vk call vkAllocateCommandBuffers failed: {}", vr);
        return nullptr;
    }
    return make_shared<CommandBufferVulkan>(_device, this, std::move(mPool), cmdBuf);
}

void QueueVulkan::Submit(std::span<CommandBuffer*> buffers, Nullable<Fence> singalFence) noexcept {
    if (buffers.size() >= std::numeric_limits<uint32_t>::max()) {
        RADRAY_ABORT("vk too many args");
        return;
    }
    VkFence rawSignalFence = VK_NULL_HANDLE;
    if (singalFence) {
        rawSignalFence = static_cast<FenceVulkan*>(singalFence.Value())->_fence;
    }
    vector<VkCommandBuffer> cmdBufs;
    for (auto i : buffers) {
        auto v = static_cast<CommandBufferVulkan*>(i);
        cmdBufs.emplace_back(v->_cmdBuffer);
    }
    VkSubmitInfo info{
        VK_STRUCTURE_TYPE_SUBMIT_INFO,
        nullptr,
        0,
        nullptr,
        nullptr,
        static_cast<uint32_t>(cmdBufs.size()),
        cmdBufs.data(),
        0,
        nullptr};
    _device->CallVk(&FTbVk::vkQueueSubmit, _queue, 1, &info, rawSignalFence);
}

void QueueVulkan::Wait() noexcept {
    if (auto vr = _device->CallVk(&FTbVk::vkQueueWaitIdle, _queue);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vk call vkQueueWaitIdle failed: {}", vr);
    }
}

void QueueVulkan::WaitFences(std::span<Fence*> fences) noexcept {
    if (fences.size() >= std::numeric_limits<uint32_t>::max()) {
        RADRAY_ABORT("vk too many args");
        return;
    }
    vector<VkFence> f;
    f.reserve(fences.size());
    for (auto i : fences) {
        auto v = static_cast<FenceVulkan*>(i);
        f.emplace_back(v->_fence);
    }
    _device->CallVk(&FTbVk::vkWaitForFences, (uint32_t)f.size(), f.data(), VK_TRUE, UINT64_MAX);
}

}  // namespace radray::render::vulkan
