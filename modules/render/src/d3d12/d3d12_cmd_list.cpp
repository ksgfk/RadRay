#include "d3d12_cmd_list.h"

#include "d3d12_device.h"
#include "d3d12_descriptor_heap.h"
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
    _cmdAlloc->Reset();
    _cmdList->Reset(_cmdAlloc.Get(), nullptr);
    ID3D12DescriptorHeap* heaps[] = {_device->_gpuResHeap->GetNative(), _device->_gpuSamplerHeap->GetNative()};
    _cmdList->SetDescriptorHeaps((UINT)radray::ArrayLength(heaps), heaps);
}

void CmdListD3D12::End() noexcept {
    _cmdList->Close();
}

void CmdListD3D12::ResourceBarrier(const ResourceBarriers& barriers) noexcept {
    vector<D3D12_RESOURCE_BARRIER> rawBarriers;
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

void CmdListD3D12::TransitionResource(std::span<TransitionBufferDescriptor> buffers, std::span<TransitionTextureDescriptor> textures) noexcept {
    RADRAY_UNIMPLEMENTED();
}

void CmdListD3D12::CopyBuffer(Buffer* src_, uint64_t srcOffset, Buffer* dst_, uint64_t dstOffset, uint64_t size) noexcept {
    BufferD3D12* src = static_cast<BufferD3D12*>(src_);
    BufferD3D12* dst = static_cast<BufferD3D12*>(dst_);
    _cmdList->CopyBufferRegion(dst->_buf.Get(), dstOffset, src->_buf.Get(), srcOffset, size);
}

void CmdListD3D12::CopyTexture(Buffer* src, uint64_t srcOffset, Texture* dst, uint32_t mipLevel, uint32_t arrayLayer, uint32_t layerCount) noexcept {
    uint32_t subresource = SubresourceIndex(
        mipLevel,
        arrayLayer,
        0,
        1,
        layerCount);
    BufferD3D12* srcBuf = static_cast<BufferD3D12*>(src);
    TextureD3D12* dstTex = static_cast<TextureD3D12*>(dst);
    D3D12_RESOURCE_DESC& dstDesc = dstTex->_desc;
    D3D12_TEXTURE_COPY_LOCATION cpSrc{};
    cpSrc.pResource = srcBuf->_buf.Get();
    cpSrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    UINT row;
    UINT64 rowSize, total;
    _device->_device->GetCopyableFootprints(
        &dstDesc,
        subresource,
        1,
        srcOffset,
        &cpSrc.PlacedFootprint,
        &row,
        &rowSize,
        &total);
    RADRAY_DEBUG_LOG("d3d12 CmdListD3D12::CopyTexture row:{} rowSize:{} total:{}", row, rowSize, total);
    cpSrc.PlacedFootprint.Offset = srcOffset;
    D3D12_TEXTURE_COPY_LOCATION cpDst{};
    cpDst.pResource = dstTex->_tex.Get();
    cpDst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cpDst.SubresourceIndex = subresource;
    _cmdList->CopyTextureRegion(&cpDst, 0, 0, 0, &cpSrc, nullptr);
}

Nullable<unique_ptr<CommandEncoder>> CmdListD3D12::BeginRenderPass(const RenderPassDesc& desc) noexcept {
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
    vector<D3D12_RENDER_PASS_RENDER_TARGET_DESC> rtDescs;
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
        rtDesc.cpuDescriptor = v->_desc.heapView.HandleCpu();
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
        dsDesc.cpuDescriptor = v->_desc.heapView.HandleCpu();
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
    return {make_unique<CmdRenderPassD3D12>(this)};
}

void CmdListD3D12::EndRenderPass(unique_ptr<CommandEncoder> encoder) noexcept {
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
    TrySetRootSig(sig);
}

void CmdRenderPassD3D12::BindPipelineState(GraphicsPipelineState* pso) noexcept {
    GraphicsPsoD3D12* psoD3D12 = static_cast<GraphicsPsoD3D12*>(pso);
    _cmdList->_cmdList->IASetPrimitiveTopology(psoD3D12->_topo);
    _cmdList->_cmdList->SetPipelineState(psoD3D12->_pso.Get());
}

void CmdRenderPassD3D12::BindDescriptorSet(uint32_t slot, DescriptorSet* descSet) noexcept {
    if (_bindRootSig == nullptr) {
        RADRAY_ERR_LOG("d3d12 cannot BindDescriptorSet, root signature not bound");
        return;
    }
    RootSigD3D12* sig = static_cast<RootSigD3D12*>(_bindRootSig);
    GpuDescriptorHeapView* heapView = static_cast<GpuDescriptorHeapView*>(descSet);
    if (slot >= sig->_bindDescriptors.size()) {
        RADRAY_ABORT("d3d12 cannot BindDescriptorSet, param 'slot' out of range {} of {}", slot, sig->_bindDescriptors.size());
        return;
    }
    UINT rootParamIndex = sig->_bindDescStart + slot;
    D3D12_GPU_DESCRIPTOR_HANDLE desc = heapView->_heapView.HandleGpu();
    _cmdList->_cmdList->SetGraphicsRootDescriptorTable(rootParamIndex, desc);
}

void CmdRenderPassD3D12::PushConstants(uint32_t slot, const void* data, size_t length) noexcept {
    if (_bindRootSig == nullptr) {
        RADRAY_ERR_LOG("d3d12 cannot PushConstants, root signature not bound");
        return;
    }
    RootSigD3D12* sig = static_cast<RootSigD3D12*>(_bindRootSig);
    if (slot >= sig->_rootConstants.size()) {
        RADRAY_ABORT("d3d12 cannot PushConstants, param 'slot' out of range {} of {}", slot, sig->_rootConstants.size());
        return;
    }
    uint32_t size = sig->_rootConstants[slot].Size;
    if (length > size) {
        RADRAY_ERR_LOG("d3d12 cannot PushConstants, param 'length' too large {}, required {}", length, size);
        return;
    }
    UINT rootParamIndex = sig->_rootConstStart + slot;
    _cmdList->_cmdList->SetGraphicsRoot32BitConstants(rootParamIndex, (UINT)length / 4, data, 0);
}

void CmdRenderPassD3D12::BindRootDescriptor(uint32_t slot, ResourceView* view) noexcept {
    if (_bindRootSig == nullptr) {
        RADRAY_ERR_LOG("d3d12 cannot BindRootDescriptor, root signature not bound");
        return;
    }
    RootSigD3D12* sig = static_cast<RootSigD3D12*>(_bindRootSig);
    if (slot >= sig->_rootDescriptors.size()) {
        RADRAY_ABORT("d3d12 cannot BindRootDescriptor, param 'slot' out of range {} of {}", slot, sig->_rootDescriptors.size());
        return;
    }
    UINT rootParamIndex = sig->_rootDescStart + slot;
    ResourceViewD3D12* viewD3D12 = static_cast<ResourceViewD3D12*>(view);
    ResourceView::Type resViewtype = viewD3D12->GetViewType();
    if (resViewtype == ResourceView::Type::Buffer) {
        BufferViewD3D12* bufferView = static_cast<BufferViewD3D12*>(view);
        D3D12_GPU_VIRTUAL_ADDRESS gpuAddr = bufferView->_desc.buffer->_gpuAddr + bufferView->_desc.offset;
        ResourceType resType = bufferView->_desc.type;
        switch (resType) {
            case ResourceType::Buffer:
                _cmdList->_cmdList->SetGraphicsRootShaderResourceView(rootParamIndex, gpuAddr);
                break;
            case ResourceType::CBuffer:
            case ResourceType::PushConstant:
                _cmdList->_cmdList->SetGraphicsRootConstantBufferView(rootParamIndex, gpuAddr);
                break;
            case ResourceType::BufferRW:
                _cmdList->_cmdList->SetGraphicsRootUnorderedAccessView(rootParamIndex, gpuAddr);
                break;
            default:
                RADRAY_ERR_LOG("d3d12 cannot BindRootDescriptor, unsupported buffer type {}", resType);
                break;
        }
    } else if (resViewtype == ResourceView::Type::Texture) {
        RADRAY_ERR_LOG("d3d12 cannot bind texture as root descriptor");
    } else {
        RADRAY_ERR_LOG("d3d12 cannot BindRootDescriptor, unsupported view type");
    }
}

void CmdRenderPassD3D12::BindVertexBuffers(std::span<VertexBufferView> vbvs) noexcept {
    if (vbvs.size() == 0) {
        return;
    }
    vector<D3D12_VERTEX_BUFFER_VIEW> rawVbvs;
    rawVbvs.reserve(vbvs.size());
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
    view.Format = stride == 1 ? DXGI_FORMAT_R8_UINT : stride == 2 ? DXGI_FORMAT_R16_UINT
                                                                  : DXGI_FORMAT_R32_UINT;
    _cmdList->_cmdList->IASetIndexBuffer(&view);
}

void CmdRenderPassD3D12::Draw(uint32_t vertexCount, uint32_t firstVertex) noexcept {
    _cmdList->_cmdList->DrawInstanced(vertexCount, 1, firstVertex, 0);
}

void CmdRenderPassD3D12::DrawIndexed(uint32_t indexCount, uint32_t firstIndex, uint32_t firstVertex) noexcept {
    _cmdList->_cmdList->DrawIndexedInstanced(indexCount, 1, firstIndex, firstVertex, 0);
}

void CmdRenderPassD3D12::TrySetRootSig(RootSigD3D12* rootSig) noexcept {
    if (_bindRootSig != rootSig) {
        _cmdList->_cmdList->SetGraphicsRootSignature(rootSig->_rootSig.Get());
        _bindRootSig = rootSig;
    }
}

}  // namespace radray::render::d3d12
