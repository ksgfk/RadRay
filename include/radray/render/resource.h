#pragma once

#include <radray/render/common.h>

namespace radray::render {

class TextureCreateDescriptor {
public:
    std::string_view Name;
    TextureDimension Dim;
    uint32_t Width;
    uint32_t Height;
    uint32_t DepthOrArraySize;
    uint32_t MipLevels;
    uint32_t SampleCount;
    TextureFormat Format;
    TextureUses Usage;
    ResourceHints Hints;
};

class Resource : public RenderBase {
public:
    ~Resource() noexcept override = default;

    virtual ResourceType GetType() const noexcept = 0;
    virtual ResourceStates GetInitState() const noexcept = 0;
};

class Texture : public Resource {
public:
    ~Texture() noexcept override = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Texture; }
};

class Buffer : public Resource {
public:
    ~Buffer() noexcept override = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::Buffer; }

    virtual uint64_t GetSize() const noexcept = 0;

    virtual Nullable<void> Map(uint64_t offset, uint64_t size) noexcept = 0;

    virtual void Unmap() noexcept = 0;
};

class ResourceView : public RenderBase {
public:
    enum class [[deprecated]] Type {
        Buffer,
        Texture
    };

    ~ResourceView() noexcept override = default;

    RenderObjectTags GetTag() const noexcept override { return RenderObjectTag::ResourceView; }

    [[deprecated]] virtual ResourceView::Type GetViewType() const noexcept = 0;
};

class BufferView : public ResourceView {
public:
    ~BufferView() noexcept override = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::BufferView; }
};

class TextureView : public ResourceView {
public:
    ~TextureView() noexcept override = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::TextureView; }
};

}  // namespace radray::render
