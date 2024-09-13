#pragma once

#include <variant>

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Device;
class DescriptorHeap;

class Texture {
public:
    Texture(
        Device* device,
        const D3D12_RESOURCE_DESC& resDesc,
        D3D12_RESOURCE_STATES initState,
        const D3D12MA::ALLOCATION_DESC& allocDesc);

    Texture(
        const ComPtr<ID3D12Resource>& res,
        D3D12_RESOURCE_STATES initState);

public:
    D3D12_RESOURCE_DESC desc;
    D3D12_RESOURCE_STATES initState;
    ComPtr<ID3D12Resource> texture;
    ComPtr<D3D12MA::Allocation> alloc;
};

class TextureView {
public:
    using ViewDesc = std::variant<
        D3D12_RENDER_TARGET_VIEW_DESC,
        D3D12_DEPTH_STENCIL_VIEW_DESC,
        D3D12_SHADER_RESOURCE_VIEW_DESC,
        D3D12_UNORDERED_ACCESS_VIEW_DESC>;

    TextureView(DescriptorHeap* heap, Texture* tex, const ViewDesc& desc);
    ~TextureView() noexcept;

public:
    ViewDesc desc;
    DescriptorHeap* heap;
    Texture* tex;
    UINT index;
};

}  // namespace radray::rhi::d3d12
