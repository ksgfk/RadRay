#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class RenderPipelineState {
public:
    RenderPipelineState(MTL::Device* device, const MTL::RenderPipelineDescriptor* desc);
    ~RenderPipelineState() noexcept;

public:
    MTL::RenderPipelineState* pso;
};

}  // namespace radray::rhi::metal
