#pragma once

#include <radray/render/common.h>

namespace radray::render {

class Resource : public RenderBase {
public:
    ~Resource() noexcept override = default;

    virtual ResourceType GetType() const noexcept = 0;
    virtual ResourceStates GetInitState() const noexcept = 0;
};

class Texture : public Resource {
public:
    ~Texture() noexcept override = default;
};

class Buffer : public Resource {
public:
    ~Buffer() noexcept override = default;

    virtual uint64_t GetSize() const noexcept = 0;

    virtual Nullable<void> Map(uint64_t offset, uint64_t size) noexcept = 0;

    virtual void Unmap() noexcept = 0;
};

class ResourceView : public RenderBase {
public:
    enum class Type {
        Buffer,
        Texture
    };

    ~ResourceView() noexcept override = default;

    virtual ResourceView::Type GetViewType() const noexcept = 0;
};

}  // namespace radray::render
