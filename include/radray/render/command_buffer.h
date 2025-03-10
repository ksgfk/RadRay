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
    ResourceView* Target;
    LoadAction Load;
    StoreAction Store;
    ColorClearValue ClearValue;
};

inline ColorAttachment DefaultColorAttachment(
    ResourceView* rtView,
    ColorClearValue clear = {0.0f, 0.0f, 0.0f, 1.0f}) noexcept {
    return {
        .Target = rtView,
        .Load = LoadAction::Clear,
        .Store = StoreAction::Store,
        .ClearValue = clear,
    };
}

struct DepthStencilAttachment {
    ResourceView* Target;
    LoadAction DepthLoad;
    StoreAction DepthStore;
    LoadAction StencilLoad;
    StoreAction StencilStore;
    DepthStencilClearValue ClearValue;
};

inline DepthStencilAttachment DefaultDepthStencilAttachment(
    ResourceView* depthView,
    DepthStencilClearValue clear = {1.0f, 0}) noexcept {
    return {
        .Target = depthView,
        .DepthLoad = LoadAction::Clear,
        .DepthStore = StoreAction::Store,
        .StencilLoad = LoadAction::Clear,
        .StencilStore = StoreAction::Store,
        .ClearValue = clear,
    };
}

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
