#pragma once

#include <radray/render/command_buffer.h>
#include <radray/render/command_encoder.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class CmdListD3D12 : public CommandBuffer {
public:
    CmdListD3D12(
        ComPtr<ID3D12CommandAllocator> cmdAlloc,
        ComPtr<ID3D12GraphicsCommandList> cmdList,
        D3D12_COMMAND_LIST_TYPE type) noexcept
        : _cmdAlloc(std::move(cmdAlloc)),
          _cmdList(std::move(cmdList)),
          _type(type) {}
    ~CmdListD3D12() noexcept override = default;

    bool IsValid() const noexcept override { return _cmdList.Get() != nullptr; }
    void Destroy() noexcept override;

    void Begin() noexcept override;

    void End() noexcept override;

    void ResourceBarrier(const ResourceBarriers& barriers) noexcept override;

    void CopyBuffer(Buffer* src, uint64_t srcOffset, Buffer* dst, uint64_t dstOffset, uint64_t size) noexcept override;

    Nullable<radray::unique_ptr<CommandEncoder>> BeginRenderPass(const RenderPassDesc& desc) noexcept override;

    void EndRenderPass(radray::unique_ptr<CommandEncoder> encoder) noexcept override;

public:
    ComPtr<ID3D12CommandAllocator> _cmdAlloc;
    ComPtr<ID3D12GraphicsCommandList> _cmdList;
    D3D12_COMMAND_LIST_TYPE _type;

    bool _isRenderPassActive{false};
};

class CmdRenderPassD3D12 : public CommandEncoder {
public:
    explicit CmdRenderPassD3D12(CmdListD3D12* cmdList) noexcept : _cmdList(cmdList) {}
    ~CmdRenderPassD3D12() noexcept override = default;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;

    void SetViewport(Viewport viewport) noexcept override;

    void SetScissor(Scissor scissor) noexcept override;

    void BindRootSignature(RootSignature* rootSig) noexcept override;

    void BindPipelineState(GraphicsPipelineState* pso) noexcept override;

    void BindDescriptorSet(uint32_t set, DescriptorSet* descSet) noexcept override;

    void PushConstants(uint32_t slot, const void* data, size_t length) noexcept override;

    void BindRootDescriptor(uint32_t slot, ResourceView* view) noexcept override;

    void BindVertexBuffers(std::span<VertexBufferView> vbvs) noexcept override;

    void BindIndexBuffer(Buffer* buffer, uint32_t stride, uint32_t offset) noexcept override;

    void Draw(uint32_t vertexCount, uint32_t firstVertex) noexcept override;

    void DrawIndexed(uint32_t indexCount, uint32_t firstIndex, uint32_t firstVertex) noexcept override;

public:
    void TrySetRootSig(RootSigD3D12* rootSig) noexcept;

    CmdListD3D12* _cmdList;

    RootSigD3D12* _bindRootSig{nullptr};
};

}  // namespace radray::render::d3d12
