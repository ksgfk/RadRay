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
    std::span<BufferBarrier> buffers;
    std::span<TextureBarrier> textures;
};

class CommandBuffer : public RenderBase {
public:
    virtual ~CommandBuffer() noexcept = default;

    virtual void Begin() noexcept = 0;

    virtual void End() noexcept = 0;

    virtual void ResourceBarrier(const ResourceBarriers& barriers) noexcept = 0;

    virtual void CopyBuffer(Buffer* src, uint64_t srcOffset, Buffer* dst, uint64_t dstOffset, uint64_t size) noexcept = 0;
};

}  // namespace radray::render
