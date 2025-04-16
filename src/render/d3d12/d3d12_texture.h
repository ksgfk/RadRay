#pragma once

#include <radray/render/resource.h>
#include "d3d12_helper.h"
#include "d3d12_descriptor_heap.h"

namespace radray::render::d3d12 {

class TextureD3D12 : public Texture {
public:
    TextureD3D12(
        ComPtr<ID3D12Resource> tex,
        ComPtr<D3D12MA::Allocation> alloc,
        const D3D12_RESOURCE_STATES& initState,
        ResourceType type,
        DXGI_FORMAT rawFormat) noexcept;
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
    DXGI_FORMAT _rawFormat;
};

struct TextureViewD3D12Desc {
    TextureD3D12* texture;
    DescriptorHeapView heapView;
    CpuDescriptorAllocator* heapAlloc;
    ResourceType type;
    DXGI_FORMAT format;
    TextureDimension dim;
    uint32_t baseArrayLayer;
    uint32_t arrayLayerCount;
    uint32_t baseMipLevel;
    uint32_t mipLevelCount;
};

class TextureViewD3D12 : public ResourceViewD3D12 {
public:
    explicit TextureViewD3D12(const TextureViewD3D12Desc& desc) noexcept
        : ResourceViewD3D12(TextureViewD3D12::Type::Texture),
          _desc(desc) {}
    ~TextureViewD3D12() noexcept override;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;

public:
    TextureViewD3D12Desc _desc;
};

}  // namespace radray::render::d3d12
