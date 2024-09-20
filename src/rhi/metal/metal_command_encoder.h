#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class RenderCommandEncoder {
public:
    RenderCommandEncoder(MTL::CommandBuffer* cmdBuffer, MTL::RenderPassDescriptor* desc);
    ~RenderCommandEncoder() noexcept;

public:
    MTL::RenderCommandEncoder* encoder;
};

}  // namespace radray::rhi::metal
