#include "d3d12_cmd_list.h"

#include "d3d12_buffer.h"
#include "d3d12_texture.h"
#include "d3d12_descriptor_heap.h"

namespace radray::render::d3d12 {

void CmdListD3D12::Destroy() noexcept {
    _cmdList = nullptr;
}

void CmdListD3D12::Begin() noexcept {
    _cmdList->Reset(_attachAlloc, nullptr);
    if (_type != D3D12_COMMAND_LIST_TYPE_COPY) {
        ID3D12DescriptorHeap* heaps[] = {_cbvSrvUavHeaps->Get(), _samplerHeaps->Get()};
        _cmdList->SetDescriptorHeaps(ArrayLength(heaps), heaps);
    }
}

void CmdListD3D12::End() noexcept {
    _cmdList->Close();
}

void CmdListD3D12::ResourceBarrier(const ResourceBarriers& barriers) noexcept {
    radray::vector<D3D12_RESOURCE_BARRIER> rawBarriers;
    rawBarriers.reserve(barriers.buffers.size() + barriers.textures.size());
    for (const BufferBarrier& bb : barriers.buffers) {
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
    for (const TextureBarrier& tb : barriers.textures) {
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

}  // namespace radray::render::d3d12
