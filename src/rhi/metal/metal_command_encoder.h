#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class CommandEncoder {
public:
    CommandEncoder(MTL::CommandBuffer* cmdBuffer, MTL::RenderPassDescriptor* desc);
    ~CommandEncoder() noexcept;

public:
    MTL::CommandEncoder* encoder;
};

}  // namespace radray::rhi::metal
