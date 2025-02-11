#pragma once

#include <radray/render/root_signature.h>
#include <radray/render/descriptor_set.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class RootConst {
public:
    radray::string _name;
    uint32_t _point;
    uint32_t _space;
};
class CBufferView {
public:
    radray::string _name;
    uint32_t _point;
    uint32_t _space;
};
class DescElem {
public:
    radray::string _name;
    ShaderResourceType _type;
    uint32_t _point;
    uint32_t _space;
    uint32_t _count;
};
class DescTable {
public:
    radray::vector<DescElem> _elems;
};

class RootSigD3D12 : public RootSignature {
public:
    explicit RootSigD3D12(ComPtr<ID3D12RootSignature>) noexcept;
    ~RootSigD3D12() noexcept override = default;

    bool IsValid() const noexcept override { return _rootSig.Get() != nullptr; }
    void Destroy() noexcept override;

    uint32_t GetDescriptorSetCount() const noexcept override;

    uint32_t GetConstantBufferSlotCount() const noexcept override;

    radray::vector<DescriptorLayout> GetDescriptorSetLayout(uint32_t set) const noexcept override;

    RootSignatureConstantBufferSlotInfo GetConstantBufferSlotInfo(uint32_t slot) const noexcept override;

public:
    ComPtr<ID3D12RootSignature> _rootSig;
    radray::vector<RootConst> _rootConsts;
    radray::vector<CBufferView> _cbufferViews;
    radray::vector<DescTable> _resDescTables;
    radray::vector<DescTable> _samplerDescTables;
};

class GpuDescriptorHeapView : public DescriptorSet {
public:
    GpuDescriptorHeapView(
        DescriptorHeap* shaderResHeap,
        DescriptorHeap* samplerHeap,
        uint32_t shaderResStart,
        uint32_t shaderResCount,
        uint32_t samplerStart,
        uint32_t samplerCount) noexcept
        : _shaderResHeap(shaderResHeap),
          _samplerHeap(samplerHeap),
          _shaderResStart(shaderResStart),
          _shaderResCount(shaderResCount),
          _samplerStart(samplerStart),
          _samplerCount(samplerCount) {}
    ~GpuDescriptorHeapView() noexcept override;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;

public:
    Nullable<DescriptorHeap> _shaderResHeap;
    Nullable<DescriptorHeap> _samplerHeap;
    uint32_t _shaderResStart;
    uint32_t _shaderResCount;
    uint32_t _samplerStart;
    uint32_t _samplerCount;
};

}  // namespace radray::render::d3d12
