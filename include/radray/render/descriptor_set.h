#pragma once

#include <radray/render/common.h>

namespace radray::render {

class DescriptorSet : public RenderBase {
public:
    virtual ~DescriptorSet() noexcept = default;
};

}  // namespace radray::render
