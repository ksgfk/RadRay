#pragma once

#include <radray/render/common.h>

namespace radray::render {

class DescriptorLayout {
public:
    radray::string Name;
    uint32_t Set;
    uint32_t Slot;
    ShaderResourceType Type;
    uint32_t Count;
};

class DescriptorSet : public RenderBase {
public:
    virtual ~DescriptorSet() noexcept = default;
};

}  // namespace radray::render
