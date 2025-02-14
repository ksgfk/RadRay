#include "d3d12_cmd_list.h"

#include "d3d12_buffer.h"
#include "d3d12_texture.h"
#include "d3d12_descriptor_heap.h"
#include "d3d12_root_sig.h"
#include "d3d12_pso.h"

namespace radray::render::d3d12 {

void CmdListD3D12::Destroy() noexcept {
    _cmdList = nullptr;
}

void CmdListD3D12::Begin() noexcept {
    _cmdList->Reset(_attachAlloc, nullptr);
    if (_type != D3D12_COMMAND_LIST_TYPE_COPY) {
        ID3D12DescriptorHeap* heaps[] = {_cbvSrvUavHeaps->Get(), _samplerHeaps->Get()};
        _cmdList->SetDescriptorHeaps((UINT)ArrayLength(heaps), heaps);
    }
}

void CmdListD3D12::End() noexcept {
    _cmdList->Close();
}

void CmdListD3D12::ResourceBarrier(const ResourceBarriers& barriers) noexcept {
    radray::vector<D3D12_RESOURCE_BARRIER> rawBarriers;
    rawBarriers.reserve(barriers.Buffers.size() + barriers.Textures.size());
    for (const BufferBarrier& bb : barriers.Buffers) {
        BufferD3D12* buf = static_cast<BufferD3D12*>(bb.Buffer);
        D3D12_RESOURCE_BARRIER raw{};
        if (bb.Before == ResourceState::UnorderedAccess && bb.After == ResourceState::UnorderedAccess) {
            raw.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            raw.UAV.pResource = buf->_buf.Get();
        } else {
            if (bb.Before == bb.After) {
                continue;
            }
            raw.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            raw.Transition.pResource = buf->_buf.Get();
            raw.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            raw.Transition.StateBefore = MapTypeResStates(bb.Before);
            raw.Transition.StateAfter = MapTypeResStates(bb.After);
        }
        rawBarriers.push_back(raw);
    }
    for (const TextureBarrier& tb : barriers.Textures) {
        TextureD3D12* tex = static_cast<TextureD3D12*>(tb.Texture);
        D3D12_RESOURCE_BARRIER raw{};
        if (tb.Before == ResourceState::UnorderedAccess && tb.After == ResourceState::UnorderedAccess) {
            raw.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            raw.UAV.pResource = tex->_tex.Get();
        } else {
            if (tb.Before == tb.After) {
                continue;
            }
            raw.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            raw.Transition.pResource = tex->_tex.Get();
            if (tb.IsSubresourceBarrier) {
                raw.Transition.Subresource = SubresourceIndex(
                    tb.MipLevel,
                    tb.ArrayLayer,
                    0,
                    tex->_desc.MipLevels,
                    tex->_desc.DepthOrArraySize);
            } else {
                raw.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            raw.Transition.StateBefore = MapTypeResStates(tb.Before);
            raw.Transition.StateAfter = MapTypeResStates(tb.After);
            // D3D12 COMMON 和 PRESENT flag 完全一致
            if (raw.Transition.StateBefore == D3D12_RESOURCE_STATE_COMMON && raw.Transition.StateAfter == D3D12_RESOURCE_STATE_COMMON) {
                if (tb.Before == ResourceState::Present || tb.After == ResourceState::Present) {
                    continue;
                }
            }
        }
        rawBarriers.push_back(raw);
    }
    if (!rawBarriers.empty()) {
        _cmdList->ResourceBarrier(static_cast<UINT>(rawBarriers.size()), rawBarriers.data());
    }
}

void CmdListD3D12::CopyBuffer(Buffer* src_, uint64_t srcOffset, Buffer* dst_, uint64_t dstOffset, uint64_t size) noexcept {
    BufferD3D12* src = static_cast<BufferD3D12*>(src_);
    BufferD3D12* dst = static_cast<BufferD3D12*>(dst_);
    _cmdList->CopyBufferRegion(dst->_buf.Get(), dstOffset, src->_buf.Get(), srcOffset, size);
}

Nullable<radray::unique_ptr<CommandEncoder>> CmdListD3D12::BeginRenderPass(const RenderPassDesc& desc) noexcept {
    if (_isRenderPassActive) {
        RADRAY_ERR_LOG("Render pass already active, cannot begin another render pass");
        return nullptr;
    }
    ComPtr<ID3D12GraphicsCommandList4> cmdList4;
    if (HRESULT hr = _cmdList->QueryInterface(IID_PPV_ARGS(cmdList4.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ERR_LOG("ID3D12GraphicsCommandList cannot convert to ID3D12GraphicsCommandList4");
        return nullptr;
    }
    radray::vector<D3D12_RENDER_PASS_RENDER_TARGET_DESC> rtDescs;
    rtDescs.reserve(desc.ColorAttachments.size());
    for (const ColorAttachment& color : desc.ColorAttachments) {
        auto v = static_cast<TextureViewD3D12*>(color.Target);
        D3D12_CLEAR_VALUE clearColor{};
        clearColor.Format = v->_desc.format;
        clearColor.Color[0] = color.ClearValue.R;
        clearColor.Color[1] = color.ClearValue.G;
        clearColor.Color[2] = color.ClearValue.B;
        clearColor.Color[3] = color.ClearValue.A;
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE beginningAccess = MapType(color.Load);
        D3D12_RENDER_PASS_ENDING_ACCESS_TYPE endingAccess = MapType(color.Store);
        auto& rtDesc = rtDescs.emplace_back(D3D12_RENDER_PASS_RENDER_TARGET_DESC{});
        rtDesc.cpuDescriptor = v->_desc.heap->HandleCpu(v->_desc.heapIndex);
        rtDesc.BeginningAccess.Type = beginningAccess;
        rtDesc.BeginningAccess.Clear.ClearValue = clearColor;
        rtDesc.EndingAccess.Type = endingAccess;
    }
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC dsDesc{};
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDsDesc = nullptr;
    if (desc.DepthStencilAttachment.has_value()) {
        const DepthStencilAttachment& depthStencil = desc.DepthStencilAttachment.value();
        auto v = static_cast<TextureViewD3D12*>(depthStencil.Target);
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE depthBeginningAccess = MapType(depthStencil.DepthLoad);
        D3D12_RENDER_PASS_ENDING_ACCESS_TYPE depthEndingAccess = MapType(depthStencil.DepthStore);
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE stencilBeginningAccess = MapType(depthStencil.StencilLoad);
        D3D12_RENDER_PASS_ENDING_ACCESS_TYPE stencilEndingAccess = MapType(depthStencil.StencilStore);
        D3D12_CLEAR_VALUE clear{};
        clear.Format = v->_desc.format;
        clear.DepthStencil.Depth = depthStencil.ClearValue.Depth;
        clear.DepthStencil.Stencil = depthStencil.ClearValue.Stencil;
        dsDesc.cpuDescriptor = v->_desc.heap->HandleCpu(v->_desc.heapIndex);
        dsDesc.DepthBeginningAccess.Type = depthBeginningAccess;
        dsDesc.DepthBeginningAccess.Clear.ClearValue = clear;
        dsDesc.DepthEndingAccess.Type = depthEndingAccess;
        dsDesc.StencilBeginningAccess.Type = stencilBeginningAccess;
        dsDesc.StencilBeginningAccess.Clear.ClearValue = clear;
        dsDesc.StencilEndingAccess.Type = stencilEndingAccess;
        pDsDesc = &dsDesc;
    }
    cmdList4->BeginRenderPass((UINT32)rtDescs.size(), rtDescs.data(), pDsDesc, D3D12_RENDER_PASS_FLAG_NONE);
    _isRenderPassActive = true;
    _bindRootSig = nullptr;
    return {radray::make_unique<CmdRenderPassD3D12>(this)};
}

void CmdListD3D12::EndRenderPass(radray::unique_ptr<CommandEncoder> encoder) noexcept {
    CmdRenderPassD3D12* pass = static_cast<CmdRenderPassD3D12*>(encoder.get());
    if (pass->_cmdList != this) {
        RADRAY_ABORT("Render pass does not belong to this command list");
        return;
    }
    ComPtr<ID3D12GraphicsCommandList4> cmdList4;
    if (HRESULT hr = _cmdList->QueryInterface(IID_PPV_ARGS(cmdList4.GetAddressOf()));
        FAILED(hr)) {
        RADRAY_ABORT("ID3D12GraphicsCommandList cannot convert to ID3D12GraphicsCommandList4");
        return;
    }
    cmdList4->EndRenderPass();
    encoder->Destroy();
    _isRenderPassActive = false;
}

void CmdListD3D12::TrySetRootSig(RootSigD3D12* rootSig) noexcept {
    if (_bindRootSig != rootSig->_rootSig) {
        _cmdList->SetGraphicsRootSignature(rootSig->_rootSig.Get());
        _bindRootSig = rootSig->_rootSig;
    }
}

bool CmdRenderPassD3D12::IsValid() const noexcept {
    return _cmdList != nullptr;
}

void CmdRenderPassD3D12::Destroy() noexcept {
    _cmdList = nullptr;
}

void CmdRenderPassD3D12::SetViewport(Viewport viewport) noexcept {
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = viewport.X;
    vp.TopLeftY = viewport.Y;
    vp.Width = viewport.Width;
    vp.Height = viewport.Height;
    vp.MinDepth = viewport.MinDepth;
    vp.MaxDepth = viewport.MaxDepth;
    _cmdList->_cmdList->RSSetViewports(1, &vp);
}

void CmdRenderPassD3D12::SetScissor(Scissor scissor) noexcept {
    D3D12_RECT rect{};
    rect.left = scissor.X;
    rect.top = scissor.Y;
    rect.right = scissor.X + scissor.Width;
    rect.bottom = scissor.Y + scissor.Height;
    _cmdList->_cmdList->RSSetScissorRects(1, &rect);
}

void CmdRenderPassD3D12::BindRootSignature(RootSignature* rootSig) noexcept {
    RootSigD3D12* sig = static_cast<RootSigD3D12*>(rootSig);
    _cmdList->TrySetRootSig(sig);
}

void CmdRenderPassD3D12::BindPipelineState(GraphicsPipelineState* pso) noexcept {
    GraphicsPsoD3D12* psoD3D12 = static_cast<GraphicsPsoD3D12*>(pso);
    _cmdList->_cmdList->IASetPrimitiveTopology(psoD3D12->_topo);
    _cmdList->_cmdList->SetPipelineState(psoD3D12->_pso.Get());
}

void CmdRenderPassD3D12::BindDescriptorSet(RootSignature* rootSig, uint32_t slot, DescriptorSet* descSet) noexcept {
    RootSigD3D12* sig = static_cast<RootSigD3D12*>(rootSig);
    GpuDescriptorHeapView* heapView = static_cast<GpuDescriptorHeapView*>(descSet);
    if (heapView->_shaderResHeap.HasValue()) {
        if (slot >= sig->_resDescTables.size()) {
            RADRAY_ABORT("d3d12 cannot SetGraphicsRootDescriptorTable, param 'slot' out of range {}", slot);
            return;
        }
    }
    if (heapView->_samplerHeap.HasValue()) {
        if (slot >= sig->_samplerDescTables.size()) {
            RADRAY_ABORT("d3d12 cannot SetGraphicsRootDescriptorTable, param 'slot' out of range {}", slot);
            return;
        }
    }
    _cmdList->TrySetRootSig(sig);
    if (heapView->_shaderResHeap.HasValue()) {
        auto heap = heapView->_shaderResHeap.Value();
        auto start = heap->HandleGpu(heapView->_shaderResStart);
        UINT rootParamIndex = sig->_resDescTables[slot]._rootParamIndex;
        _cmdList->_cmdList->SetGraphicsRootDescriptorTable(rootParamIndex, start);
    }
    if (heapView->_samplerHeap.HasValue()) {
        auto heap = heapView->_samplerHeap.Value();
        auto start = heap->HandleGpu(heapView->_samplerStart);
        UINT rootParamIndex = sig->_samplerDescTables[slot]._rootParamIndex;
        _cmdList->_cmdList->SetGraphicsRootDescriptorTable(rootParamIndex, start);
    }
}

void CmdRenderPassD3D12::PushConstants(RootSignature* rootSig, uint32_t slot, const void* data, size_t length) noexcept {
    RootSigD3D12* sig = static_cast<RootSigD3D12*>(rootSig);
    if (slot >= sig->_rootConsts.size()) {
        RADRAY_ABORT("d3d12 cannot SetGraphicsRoot32BitConstants, param 'slot' out of range {}", slot);
        return;
    }
    const RootConst& rootConst = sig->_rootConsts[slot];
    if (length < rootConst._num32BitValues * 4) {
        RADRAY_ERR_LOG("d3d12 cannot SetGraphicsRoot32BitConstants, param 'length' too small");
    }
    _cmdList->TrySetRootSig(sig);
    _cmdList->_cmdList->SetGraphicsRoot32BitConstants(rootConst._rootParamIndex, rootConst._num32BitValues, data, 0);
}

void CmdRenderPassD3D12::BindConstantBuffer(RootSignature* rootSig, uint32_t slot, Buffer* buffer, uint32_t offset) noexcept {
    RootSigD3D12* sig = static_cast<RootSigD3D12*>(rootSig);
    if (slot >= sig->_cbufferViews.size()) {
        RADRAY_ABORT("d3d12 cannot SetGraphicsRootConstantBufferView, param 'slot' out of range {}", slot);
        return;
    }
    const CBufferView& cbvd = sig->_cbufferViews[slot];
    BufferD3D12* bufferD3D12 = static_cast<BufferD3D12*>(buffer);
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddr = bufferD3D12->_gpuAddr + offset;
    _cmdList->TrySetRootSig(sig);
    _cmdList->_cmdList->SetGraphicsRootConstantBufferView(cbvd._rootParamIndex, gpuAddr);
}

void CmdRenderPassD3D12::BindVertexBuffers(std::span<VertexBufferView> vbvs) noexcept {
    if (vbvs.size() == 0) {
        return;
    }
    radray::vector<D3D12_VERTEX_BUFFER_VIEW> rawVbvs;
    rawVbvs.resize(vbvs.size());
    for (const VertexBufferView& i : vbvs) {
        D3D12_VERTEX_BUFFER_VIEW& raw = rawVbvs.emplace_back();
        BufferD3D12* buf = static_cast<BufferD3D12*>(i.Buffer);
        raw.BufferLocation = buf->_gpuAddr + i.Offset;
        raw.SizeInBytes = (UINT)buf->GetSize() - i.Offset;
        raw.StrideInBytes = i.Stride;
    }
    _cmdList->_cmdList->IASetVertexBuffers(0, (UINT)rawVbvs.size(), rawVbvs.data());
}

void CmdRenderPassD3D12::BindIndexBuffer(Buffer* buffer, uint32_t stride, uint32_t offset) noexcept {
    BufferD3D12* buf = static_cast<BufferD3D12*>(buffer);
    D3D12_INDEX_BUFFER_VIEW view{};
    view.BufferLocation = buf->_gpuAddr + offset;
    view.SizeInBytes = (UINT)buf->GetSize() - offset;
    view.Format = stride == 1 ? DXGI_FORMAT_R8_UINT : stride == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    _cmdList->_cmdList->IASetIndexBuffer(&view);
}

void CmdRenderPassD3D12::Draw(uint32_t vertexCount, uint32_t firstVertex) noexcept {
    _cmdList->_cmdList->DrawInstanced(vertexCount, 1, firstVertex, 0);
}

void CmdRenderPassD3D12::DrawIndexed(uint32_t indexCount, uint32_t firstIndex, uint32_t firstVertex) noexcept {
    _cmdList->_cmdList->DrawIndexedInstanced(indexCount, 1, firstIndex, firstVertex, 0);
}

}  // namespace radray::render::d3d12
