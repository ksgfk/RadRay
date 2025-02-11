#include "d3d12_root_sig.h"

#include "d3d12_descriptor_heap.h"

namespace radray::render::d3d12 {

RootSigD3D12::RootSigD3D12(ComPtr<ID3D12RootSignature> rootSig) noexcept : _rootSig(std::move(rootSig)) {}

void RootSigD3D12::Destroy() noexcept { _rootSig = nullptr; }

uint32_t RootSigD3D12::GetDescriptorSetCount() const noexcept {
    return _resDescTables.size() + _samplerDescTables.size();
}

uint32_t RootSigD3D12::GetConstantBufferSlotCount() const noexcept {
    return _rootConsts.size() + _cbufferViews.size();
}

radray::vector<DescriptorLayout> RootSigD3D12::GetDescriptorSetLayout(uint32_t set) const noexcept {
    if (set < _resDescTables.size()) {
        const auto& descTable = _resDescTables[set];
        radray::vector<DescriptorLayout> layouts;
        layouts.reserve(descTable._elems.size());
        for (size_t i = 0; i < descTable._elems.size(); i++) {
            const auto& elem = descTable._elems[i];
            DescriptorLayout layout;
            layout.Name = elem._name;
            layout.Set = elem._space;
            layout.Slot = i;
            layout.Type = elem._type;
            layout.Count = elem._count;
            layouts.push_back(layout);
        }
        return layouts;
    } else if (set < _resDescTables.size() + _samplerDescTables.size()) {
        const auto& descTable = _samplerDescTables[set - _resDescTables.size()];
        radray::vector<DescriptorLayout> layouts;
        layouts.reserve(descTable._elems.size());
        for (size_t i = 0; i < descTable._elems.size(); i++) {
            const auto& elem = descTable._elems[i];
            DescriptorLayout layout;
            layout.Name = elem._name;
            layout.Set = elem._space;
            layout.Slot = i;
            layout.Type = elem._type;
            layout.Count = elem._count;
            layouts.push_back(layout);
        }
        return layouts;
    } else {
        RADRAY_ABORT("out of range");
        return {};
    }
}

RootSignatureConstantBufferSlotInfo RootSigD3D12::GetConstantBufferSlotInfo(uint32_t slot) const noexcept {
    if (slot < _rootConsts.size()) {
        const auto& rootConst = _rootConsts[slot];
        return RootSignatureConstantBufferSlotInfo{rootConst._name, slot};
    } else if (slot < _rootConsts.size() + _cbufferViews.size()) {
        const auto& cbufferView = _cbufferViews[slot - _rootConsts.size()];
        return RootSignatureConstantBufferSlotInfo{cbufferView._name, slot};
    } else {
        RADRAY_ABORT("out of range");
        return {};
    }
}

//TODO:
GpuDescriptorHeapView::~GpuDescriptorHeapView() noexcept {

}

bool GpuDescriptorHeapView::IsValid() const noexcept {
    return !_shaderResHeap.HasValue() && !_samplerHeap.HasValue();
}

void GpuDescriptorHeapView::Destroy() noexcept {
    if (_shaderResHeap.HasValue()) {
        // _shaderResHeap.Value()->
    }
}

}  // namespace radray::render::d3d12
