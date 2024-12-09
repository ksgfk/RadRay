#pragma once

#include <radray/render/resource.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class BufferD3D12 : public Buffer {
public:
    BufferD3D12(
        ComPtr<ID3D12Resource> buf,
        ComPtr<D3D12MA::Allocation> alloc,
        D3D12_RESOURCE_STATES initState,
        ResourceType type) noexcept;
    ~BufferD3D12() noexcept override = default;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;
    ResourceType GetType() const noexcept override;
    ResourceStates GetInitState() const noexcept override;

public:
    ComPtr<ID3D12Resource> _buf;
    ComPtr<D3D12MA::Allocation> _alloc;
    D3D12_RESOURCE_DESC _desc;
    D3D12_RESOURCE_STATES _initState;
    ResourceType _type;
};

}  // namespace radray::render::d3d12
