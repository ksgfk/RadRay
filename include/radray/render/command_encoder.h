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

    virtual void BindPipelineState(GraphicsPipelineState* pso) noexcept = 0;

    virtual void BindDescriptorSet(RootSignature* rootSig, DescriptorSet* descSet, uint32_t set) noexcept = 0;

    virtual void PushConstants(RootSignature* rootSig, uint32_t slot, const void* data, size_t length) noexcept = 0;

    virtual void BindConstantBuffer(RootSignature* rootSig, BufferView* buffer, uint32_t slot) noexcept = 0;
};

}  // namespace radray::render
