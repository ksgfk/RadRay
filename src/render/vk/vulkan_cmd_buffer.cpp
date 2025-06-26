#include "vulkan_cmd_buffer.h"

#include <radray/render/command_encoder.h>

#include "vulkan_device.h"

namespace radray::render::vulkan {

static void DestroyCommandBufferVulkan(CommandBufferVulkan& cmdBuffer) noexcept {
    if (cmdBuffer.IsValid()) {
        cmdBuffer._device->CallVk(&FTbVk::vkFreeCommandBuffers, cmdBuffer._pool->_pool, 1, &cmdBuffer._cmdBuffer);
        cmdBuffer._cmdBuffer = VK_NULL_HANDLE;
        cmdBuffer._pool = nullptr;
    }
}

CommandBufferVulkan::~CommandBufferVulkan() noexcept {
    DestroyCommandBufferVulkan(*this);
}

bool CommandBufferVulkan::IsValid() const noexcept {
    return _cmdBuffer != VK_NULL_HANDLE;
}

void CommandBufferVulkan::Destroy() noexcept {
    DestroyCommandBufferVulkan(*this);
}

void CommandBufferVulkan::Begin() noexcept {}

void CommandBufferVulkan::End() noexcept {}

void CommandBufferVulkan::ResourceBarrier(const ResourceBarriers& barriers) noexcept {}

void CommandBufferVulkan::CopyBuffer(Buffer* src, uint64_t srcOffset, Buffer* dst, uint64_t dstOffset, uint64_t size) noexcept {}

void CommandBufferVulkan::CopyTexture(Buffer* src, uint64_t srcOffset, Texture* dst, uint32_t mipLevel, uint32_t arrayLayer, uint32_t layerCount) noexcept {}

Nullable<unique_ptr<CommandEncoder>> CommandBufferVulkan::BeginRenderPass(const RenderPassDesc& desc) noexcept {
    return nullptr;
}

void CommandBufferVulkan::EndRenderPass(unique_ptr<CommandEncoder> encoder) noexcept {}

}  // namespace radray::render::vulkan
