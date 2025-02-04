#pragma once

#include <radray/render/root_signature.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class RootConst {
public:
    RootConst(std::string_view name, uint32_t point, uint32_t space) noexcept
        : _name(name), _point(point), _space(space) {}

    std::string _name;
    uint32_t _point;
    uint32_t _space;
};
class CBufferView {
public:
    CBufferView(std::string_view name, uint32_t point, uint32_t space) noexcept
        : _name(name), _point(point), _space(space) {}

    std::string _name;
    uint32_t _point;
    uint32_t _space;
};
class DescTable {

};

class RootSigD3D12 : public RootSignature {
public:
    RootSigD3D12(ComPtr<ID3D12RootSignature>) noexcept;
    ~RootSigD3D12() noexcept override = default;

    bool IsValid() const noexcept override { return _rootSig.Get() != nullptr; }
    void Destroy() noexcept override;

public:
    ComPtr<ID3D12RootSignature> _rootSig;
};

}  // namespace radray::render::d3d12
