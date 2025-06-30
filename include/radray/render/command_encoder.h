#pragma once

#include <radray/basic_math.h>
#include <radray/render/common.h>

namespace radray::render {

struct VertexBufferView {
    Buffer* Buffer;
    uint32_t Stride;
    uint32_t Offset;
};

class CommandEncoder : public RenderBase {
public:
    virtual ~CommandEncoder() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::CmdEncoder; }

    virtual void SetViewport(Viewport viewport) noexcept = 0;

    virtual void SetScissor(Scissor scissor) noexcept = 0;

    virtual void BindRootSignature(RootSignature* rootSig) noexcept = 0;

    virtual void BindPipelineState(GraphicsPipelineState* pso) noexcept = 0;

    virtual void BindDescriptorSet(uint32_t slot, DescriptorSet* descSet) noexcept = 0;

    virtual void PushConstants(uint32_t slot, const void* data, size_t length) noexcept = 0;

    virtual void BindRootDescriptor(uint32_t slot, ResourceView* view) noexcept = 0;

    virtual void BindVertexBuffers(std::span<VertexBufferView> vbvs) noexcept = 0;

    virtual void BindIndexBuffer(Buffer* buffer, uint32_t stride, uint32_t offset) noexcept = 0;

    virtual void Draw(uint32_t vertexCount, uint32_t firstVertex) noexcept = 0;

    virtual void DrawIndexed(uint32_t indexCount, uint32_t firstIndex, uint32_t firstVertex) noexcept = 0;
};

}  // namespace radray::render
