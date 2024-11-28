#pragma once

#include <radray/render/root_signature.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

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
