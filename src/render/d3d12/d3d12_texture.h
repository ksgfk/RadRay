#pragma once

#include <radray/render/resource.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class TextureD3D12 : public Texture {
public:
    TextureD3D12(
        ComPtr<ID3D12Resource> tex,
        ComPtr<D3D12MA::Allocation> alloc,
        const D3D12_RESOURCE_STATES& initState,
        ResourceType type) noexcept;
    ~TextureD3D12() noexcept override = default;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;
    ResourceType GetType() const noexcept override;
    ResourceStates GetInitState() const noexcept override;

public:
    ComPtr<ID3D12Resource> _tex;
    ComPtr<D3D12MA::Allocation> _alloc;
    D3D12_RESOURCE_DESC _desc;
    D3D12_RESOURCE_STATES _initState;
    ResourceType _type;
};

}  // namespace radray::render::d3d12
