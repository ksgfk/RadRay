#pragma once

#include <radray/rhi/texture.h>

#include "helper.h"

namespace radray::rhi::d3d12 {

class Device;

struct TextureConstructParams {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mip;
    DXGI_FORMAT format;
    TextureDimension dim;
    ComPtr<ID3D12Resource> texture;
    ComPtr<D3D12MA::Allocation> allocaton;
    D3D12_RESOURCE_STATES initState;
};

class Texture : public ITexture {
public:
    Texture(
        std::shared_ptr<Device> device,
        const TextureConstructParams& tcParams);
    ~Texture() noexcept override = default;

    ID3D12Resource* GetResource() const noexcept { return _resource.Get(); }
    D3D12_RESOURCE_STATES GetInitState() const noexcept { return _initState; }

private:
    ComPtr<ID3D12Resource> _resource;
    ComPtr<D3D12MA::Allocation> _alloc;
    D3D12_RESOURCE_STATES _initState;
};

}  // namespace radray::rhi::d3d12
