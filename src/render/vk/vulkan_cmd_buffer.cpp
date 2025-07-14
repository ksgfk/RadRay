#include "vulkan_cmd_buffer.h"

#include <radray/render/command_encoder.h>

#include "vulkan_device.h"
#include "vulkan_cmd_queue.h"
#include "vulkan_image.h"

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

void CommandBufferVulkan::Begin() noexcept {
    if (auto vr = _device->CallVk(&FTbVk::vkResetCommandBuffer, _cmdBuffer, 0);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vk call vkResetCommandBuffer failed: {}", vr);
    }
    _pool->Reset();
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.pNext = nullptr;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    beginInfo.pInheritanceInfo = nullptr;
    if (auto vr = _device->CallVk(&FTbVk::vkBeginCommandBuffer, _cmdBuffer, &beginInfo);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vk call vkBeginCommandBuffer failed: {}", vr);
    }
}

void CommandBufferVulkan::End() noexcept {
    if (auto vr = _device->CallVk(&FTbVk::vkEndCommandBuffer, _cmdBuffer);
        vr != VK_SUCCESS) {
        RADRAY_ABORT("vk call vkEndCommandBuffer failed: {}", vr);
    }
}

void CommandBufferVulkan::ResourceBarrier(const ResourceBarriers& barriers) noexcept {
    // VkAccessFlags srcAccessFlags = 0;
    // VkAccessFlags dstAccessFlags = 0;

    // vector<VkBufferMemoryBarrier> bmbs;
    // TODO: buffer barrier
    // bmbs.reserve(barriers.Buffers.size());
    // for (const auto& i : barriers.Buffers) {
    //     VkBufferMemoryBarrier bmb{};
    //     if (i.Before.HasFlag(ResourceState::UnorderedAccess) && i.After.HasFlag(ResourceState::UnorderedAccess)) {
    //         auto& bmb = bmbs.emplace_back();
    //         bmb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    //         bmb.pNext = nullptr;
    //         bmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    //         bmb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    //         bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    //         bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    //     }
    // }

    // vector<VkImageMemoryBarrier> imbs;
    // imbs.reserve(barriers.Textures.size());
    // for (const auto& i : barriers.Textures) {
    //     VkImageMemoryBarrier* pImb = nullptr;
    //     if (i.Before.HasFlag(ResourceState::UnorderedAccess) && i.After.HasFlag(ResourceState::UnorderedAccess)) {
    //         auto& imb = imbs.emplace_back();
    //         imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    //         imb.pNext = nullptr;
    //         imb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    //         imb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    //         imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    //         imb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    //         pImb = &imb;
    //     } else {
    //         auto& imb = imbs.emplace_back();
    //         imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    //         imb.pNext = nullptr;
    //         imb.srcAccessMask = MapAccessMask(i.Before);
    //         imb.dstAccessMask = MapAccessMask(i.After);
    //         imb.oldLayout = MapImageLayout(i.Before);
    //         imb.newLayout = MapImageLayout(i.After);
    //         pImb = &imb;
    //     }
    //     if (pImb != nullptr) {
    //         auto tex = static_cast<ImageVulkan*>(i.Texture);
    //         pImb->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    //         pImb->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    //         pImb->image = tex->_image;
    //         VkImageSubresourceRange subresRange{};
    //         subresRange.aspectMask = ToImageAspectFlags(tex->_rawFormat);
    //         subresRange.baseMipLevel = i.IsSubresourceBarrier ? i.MipLevel : 0;
    //         subresRange.levelCount = i.IsSubresourceBarrier ? 1 : VK_REMAINING_MIP_LEVELS;
    //         subresRange.baseArrayLayer = i.IsSubresourceBarrier ? i.ArrayLayer : 0;
    //         subresRange.layerCount = i.IsSubresourceBarrier ? 1 : VK_REMAINING_ARRAY_LAYERS;
    //         pImb->subresourceRange = subresRange;

    //         srcAccessFlags |= pImb->srcAccessMask;
    //         dstAccessFlags |= pImb->dstAccessMask;
    //     }
    // }
    // VkPipelineStageFlags srcStageMask = DeterminePipelineStageFlags(srcAccessFlags, _queue->_type);
    // VkPipelineStageFlags dstStageMask = DeterminePipelineStageFlags(dstAccessFlags, _queue->_type);
    // if (!bmbs.empty() || !imbs.empty()) {
    //     _device->CallVk(
    //         &FTbVk::vkCmdPipelineBarrier,
    //         _cmdBuffer,
    //         srcStageMask,
    //         dstStageMask,
    //         0,
    //         0,
    //         nullptr,
    //         static_cast<uint32_t>(bmbs.size()),
    //         bmbs.data(),
    //         static_cast<uint32_t>(imbs.size()),
    //         imbs.data());
    // }
}

void CommandBufferVulkan::TransitionResource(std::span<TransitionBufferDescriptor> buffers, std::span<TransitionTextureDescriptor> textures) noexcept {
    VkAccessFlags srcAccessFlags = VK_ACCESS_NONE;
    VkAccessFlags dstAccessFlags = VK_ACCESS_NONE;

    vector<VkBufferMemoryBarrier> bmbs;

    vector<VkImageMemoryBarrier> imbs;
    for (const auto& i : textures) {
        auto tex = static_cast<ImageVulkan*>(i.Texture);
        auto& imb = imbs.emplace_back();

        imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        imb.pNext = nullptr;
        // imb.srcAccessMask = MapAccessMask(i.Before);
        // imb.dstAccessMask = MapAccessMask(i.After);
        // imb.oldLayout = MapImageLayout(i.Before);
        // imb.newLayout = MapImageLayout(i.After);
        // TextureUseToBarrier(i.Before, imb.srcStageMask, imb.srcAccessMask);
        // TextureUseToBarrier(i.After, imb.dstStageMask, imb.dstAccessMask);

        imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        imb.image = tex->_image;
        VkImageSubresourceRange subresRange{};
        subresRange.aspectMask = ToImageAspectFlags(tex->_desc._rawFormat);
        subresRange.baseMipLevel = i.IsSubresourceBarrier ? i.BaseMipLevel : 0;
        subresRange.levelCount = i.IsSubresourceBarrier ? i.MipLevelCount : VK_REMAINING_MIP_LEVELS;
        subresRange.baseArrayLayer = i.IsSubresourceBarrier ? i.BaseArrayLayer : 0;
        subresRange.layerCount = i.IsSubresourceBarrier ? i.ArrayLayerCount : VK_REMAINING_ARRAY_LAYERS;
        imb.subresourceRange = subresRange;

        srcAccessFlags |= imb.srcAccessMask;
        dstAccessFlags |= imb.dstAccessMask;
    }
}

void CommandBufferVulkan::CopyBuffer(Buffer* src, uint64_t srcOffset, Buffer* dst, uint64_t dstOffset, uint64_t size) noexcept {
    RADRAY_UNIMPLEMENTED();
}

void CommandBufferVulkan::CopyTexture(Buffer* src, uint64_t srcOffset, Texture* dst, uint32_t mipLevel, uint32_t arrayLayer, uint32_t layerCount) noexcept {
    RADRAY_UNIMPLEMENTED();
}

Nullable<unique_ptr<CommandEncoder>> CommandBufferVulkan::BeginRenderPass(const RenderPassDesc& desc) noexcept {
    RADRAY_UNIMPLEMENTED();
    return nullptr;
}

void CommandBufferVulkan::EndRenderPass(unique_ptr<CommandEncoder> encoder) noexcept {
    RADRAY_UNIMPLEMENTED();
}

}  // namespace radray::render::vulkan
