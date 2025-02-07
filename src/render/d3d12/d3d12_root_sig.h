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
    D3D12_DESCRIPTOR_RANGE_TYPE _type;
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

public:
    ComPtr<ID3D12RootSignature> _rootSig;
    radray::vector<RootConst> _rootConsts;
    radray::vector<CBufferView> _cbufferViews;
    radray::vector<DescTable> _resDescTables;
    radray::vector<DescTable> _samplerDescTables;
};

class ShaderBindTable : public DescriptorSet {
public:
    ~ShaderBindTable() noexcept override = default;


};

}  // namespace radray::render::d3d12
