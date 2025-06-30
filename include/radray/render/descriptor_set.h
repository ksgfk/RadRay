#pragma once

#include <radray/render/common.h>

namespace radray::render {

class DescriptorSet : public RenderBase {
public:
    virtual ~DescriptorSet() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::DescriptorSet; }

    virtual void SetResources(uint32_t start, std::span<ResourceView*> views) noexcept = 0;
};

}  // namespace radray::render
