#pragma once

#include <radray/basic_math.h>
#include <radray/render/common.h>

namespace radray::render {

class CommandEncoder : public RenderBase {
public:
    virtual ~CommandEncoder() noexcept = default;

    virtual void SetViewport(Viewport viewport) noexcept = 0;

    virtual void SetScissor(Scissor scissor) noexcept = 0;

    virtual void BindRootSignature(RootSignature* rootSig) noexcept = 0;

    virtual void BindDescriptorSet(DescriptorSet* descSet, uint32_t set) noexcept = 0;

    // virtual void SetPipelineState(GraphicsPipelineState* pso) noexcept = 0;
};

}  // namespace radray::render
