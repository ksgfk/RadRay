#pragma once

#include <radray/render/common.h>

namespace radray::render {

struct BufferBarrier {
    Buffer* Buffer;
    ResourceStates Before;
    ResourceStates After;
};

struct TextureBarrier {
    Texture* Texture;
    ResourceStates Before;
    ResourceStates After;
    uint32_t MipLevel;
    uint32_t ArrayLayer;
    bool IsSubresourceBarrier;
};

struct ResourceBarriers {
    std::span<BufferBarrier> Buffers;
    std::span<TextureBarrier> Textures;
};

struct ColorAttachment {
    TextureView* Target;
    LoadAction Load;
    StoreAction Store;
    ColorClearValue ClearValue; 
};

struct DepthStencilAttachment {
    TextureView* Target;
    LoadAction DepthLoad;
    StoreAction DepthStore;
    LoadAction StencilLoad;
    StoreAction StencilStore;
    DepthStencilClearValue ClearValue;
};

class RenderPassDesc {
public:
    radray::string Name;
    std::span<ColorAttachment> ColorAttachments;
    std::optional<DepthStencilAttachment> DepthStencilAttachment;
};

class CommandBuffer : public RenderBase {
public:
    virtual ~CommandBuffer() noexcept = default;

    virtual void Begin() noexcept = 0;

    virtual void End() noexcept = 0;

    virtual void ResourceBarrier(const ResourceBarriers& barriers) noexcept = 0;

    virtual void CopyBuffer(Buffer* src, uint64_t srcOffset, Buffer* dst, uint64_t dstOffset, uint64_t size) noexcept = 0;

    virtual Nullable<radray::unique_ptr<CommandEncoder>> BeginRenderPass(const RenderPassDesc& desc) noexcept = 0;

    virtual void EndRenderPass(radray::unique_ptr<CommandEncoder> encoder) noexcept = 0;
};

}  // namespace radray::render
