#pragma once

#include <radray/render/command_buffer.h>

#include "vulkan_helper.h"
#include "vulkan_cmd_pool.h"

namespace radray::render::vulkan {

class CommandBufferVulkan : public CommandBuffer {
public:
    CommandBufferVulkan(
        DeviceVulkan* device,
        QueueVulkan* queue,
        unique_ptr<CommandPoolVulkan> pool,
        VkCommandBuffer cmdBuffer) noexcept
        : _device(device),
          _queue(queue),
          _pool(std::move(pool)),
          _cmdBuffer(cmdBuffer) {};

    ~CommandBufferVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    void Begin() noexcept override;

    void End() noexcept override;

    void ResourceBarrier(const ResourceBarriers& barriers) noexcept override;

    void TransitionResource(std::span<TransitionBufferDescriptor> buffers, std::span<TransitionTextureDescriptor> textures) noexcept override;

    void CopyBuffer(Buffer* src, uint64_t srcOffset, Buffer* dst, uint64_t dstOffset, uint64_t size) noexcept override;

    void CopyTexture(Buffer* src, uint64_t srcOffset, Texture* dst, uint32_t mipLevel, uint32_t arrayLayer, uint32_t layerCount) noexcept override;

    Nullable<unique_ptr<CommandEncoder>> BeginRenderPass(const RenderPassDesc& desc) noexcept override;

    void EndRenderPass(unique_ptr<CommandEncoder> encoder) noexcept override;

public:
    DeviceVulkan* _device;
    QueueVulkan* _queue;
    unique_ptr<CommandPoolVulkan> _pool;
    VkCommandBuffer _cmdBuffer;
};

}  // namespace radray::render::vulkan
