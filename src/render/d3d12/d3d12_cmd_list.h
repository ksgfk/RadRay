#pragma once

#include <radray/render/command_buffer.h>
#include <radray/render/command_encoder.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class CmdListD3D12 : public CommandBuffer {
public:
    CmdListD3D12(
        ComPtr<ID3D12GraphicsCommandList> cmdList,
        ID3D12CommandAllocator* attachAlloc,
        D3D12_COMMAND_LIST_TYPE type,
        DescriptorHeap* cbvSrvUavHeaps,
        DescriptorHeap* samplerHeaps) noexcept
        : _cmdList(std::move(cmdList)),
          _attachAlloc(attachAlloc),
          _cbvSrvUavHeaps(cbvSrvUavHeaps),
          _samplerHeaps(samplerHeaps),
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
    ComPtr<ID3D12GraphicsCommandList> _cmdList;
    ID3D12CommandAllocator* _attachAlloc;
    DescriptorHeap* _cbvSrvUavHeaps;
    DescriptorHeap* _samplerHeaps;
    D3D12_COMMAND_LIST_TYPE _type;
};

class CmdRenderPassD3D12 : public CommandEncoder {
public:
    explicit CmdRenderPassD3D12(CmdListD3D12* cmdList) noexcept : _cmdList(cmdList) {}
    ~CmdRenderPassD3D12() noexcept override = default;

    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}

public:
    CmdListD3D12* _cmdList;
};

}  // namespace radray::render::d3d12
