#include "d3d12_root_sig.h"

#include "d3d12_descriptor_heap.h"
#include "d3d12_buffer.h"
#include "d3d12_texture.h"

namespace radray::render::d3d12 {

RootSigD3D12::RootSigD3D12(ComPtr<ID3D12RootSignature> rootSig) noexcept : _rootSig(std::move(rootSig)) {}

void RootSigD3D12::Destroy() noexcept { _rootSig = nullptr; }

uint32_t RootSigD3D12::GetDescriptorSetCount() const noexcept {
    return _resDescTables.size() + _samplerDescTables.size();
}

uint32_t RootSigD3D12::GetConstantBufferSlotCount() const noexcept {
    return _cbufferViews.size();
}

uint32_t RootSigD3D12::GetRootConstantCount() const noexcept {
    return _rootConsts.size();
}

radray::vector<DescriptorLayout> RootSigD3D12::GetDescriptorSetLayout(uint32_t set) const noexcept {
    if (set < _resDescTables.size()) {
        const auto& descTable = _resDescTables[set];
        radray::vector<DescriptorLayout> layouts;
        layouts.reserve(descTable._elems.size());
        for (size_t i = 0; i < descTable._elems.size(); i++) {
            const auto& elem = descTable._elems[i];
            layouts.emplace_back(DescriptorLayout{
                elem._name,
                elem._space,
                (uint32_t)i,
                elem._type,
                elem._count,
                elem._cbSize});
        }
        return layouts;
    } else if (set < _resDescTables.size() + _samplerDescTables.size()) {
        const auto& descTable = _samplerDescTables[set - _resDescTables.size()];
        radray::vector<DescriptorLayout> layouts;
        layouts.reserve(descTable._elems.size());
        for (size_t i = 0; i < descTable._elems.size(); i++) {
            const auto& elem = descTable._elems[i];
            layouts.emplace_back(DescriptorLayout{
                elem._name,
                elem._space,
                (uint32_t)i,
                elem._type,
                elem._count,
                elem._cbSize});
        }
        return layouts;
    } else {
        RADRAY_ABORT("out of range");
        return {};
    }
}

RootSignatureConstantBufferSlotInfo RootSigD3D12::GetConstantBufferSlotInfo(uint32_t slot) const noexcept {
    if (slot >= _cbufferViews.size()) {
        RADRAY_ABORT("out of range");
        return {};
    }
    const CBufferView& cbufferView = _cbufferViews[slot];
    return RootSignatureConstantBufferSlotInfo{cbufferView._name, cbufferView._size, slot};
}

RootSignatureRootConstantSlotInfo RootSigD3D12::GetRootConstantSlotInfo(uint32_t slot) const noexcept {
    if (slot >= _rootConsts.size()) {
        RADRAY_ABORT("out of range");
        return {};
    }
    const RootConst& rootConst = _rootConsts[slot];
    return RootSignatureRootConstantSlotInfo{rootConst._name, rootConst._num32BitValues * 4, slot};
}

static void DestroyGpuDescriptorHeapView(GpuDescriptorHeapView* v) noexcept {
    if (v->_shaderResHeap.HasValue()) {
        auto heap = v->_shaderResHeap.Value();
        heap->RecycleRange(v->_shaderResStart, v->_shaderResCount);
        v->_shaderResHeap.Release();
    }
    if (v->_samplerHeap.HasValue()) {
        auto heap = v->_samplerHeap.Value();
        heap->RecycleRange(v->_samplerStart, v->_samplerCount);
        v->_samplerHeap.Release();
    }
}

GpuDescriptorHeapView::~GpuDescriptorHeapView() noexcept {
    DestroyGpuDescriptorHeapView(this);
}

bool GpuDescriptorHeapView::IsValid() const noexcept {
    return !_shaderResHeap.HasValue() && !_samplerHeap.HasValue();
}

void GpuDescriptorHeapView::Destroy() noexcept {
    DestroyGpuDescriptorHeapView(this);
}

void GpuDescriptorHeapView::SetResources(std::span<ResourceView*> views) noexcept {
    for (ResourceView* rv : views) {
        auto type = rv->GetViewType();
        if (type == ResourceView::Type::Buffer) {
            // TODO:
            // BufferViewD3D12* bv = static_cast<BufferViewD3D12*>(rv);
            // const auto& desc = bv->_desc;
            // desc.heap->CopyTo(desc.heapIndex, 1, _shaderResHeap.Value(), _shaderResStart);
        }
    }
}

}  // namespace radray::render::d3d12
