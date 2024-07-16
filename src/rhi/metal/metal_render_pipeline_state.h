#pragma once

#include "metal_helper.h"

namespace radray::rhi::metal {

class MetalRenderPipelineState {
public:
    MetalRenderPipelineState(MTL::Device* device, MTL::RenderPipelineDescriptor* desc);
    MetalRenderPipelineState(const MetalRenderPipelineState&) = delete;
    MetalRenderPipelineState(MetalRenderPipelineState&&) = delete;
    MetalRenderPipelineState& operator=(const MetalRenderPipelineState&) = delete;
    MetalRenderPipelineState& operator=(MetalRenderPipelineState&&) = delete;
    ~MetalRenderPipelineState() noexcept;

    bool IsValid() const noexcept;

public:
    MTL::RenderPipelineState* pso;
};

}  // namespace radray::rhi::metal
