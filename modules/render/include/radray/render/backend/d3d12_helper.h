#pragma once

#ifdef RADRAY_ENABLE_D3D12

#include <optional>

#include <radray/types.h>
#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/render/common.h>
#include <radray/render/utility.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINDOWS
#define _WINDOWS
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <wrl.h>
#include <dxgi1_6.h>
#include <directx/d3dx12.h>
#include <directx/d3d12shader.h>
#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED
#include <D3D12MemAlloc.h>

namespace radray::render::d3d12 {

using Microsoft::WRL::ComPtr;

class Win32Event {
public:
    Win32Event() noexcept = default;
    ~Win32Event() noexcept;
    Win32Event(const Win32Event& other) noexcept = delete;
    Win32Event(Win32Event&& other) noexcept;
    Win32Event& operator=(Win32Event&& other) noexcept;
    Win32Event& operator=(const Win32Event& other) noexcept = delete;

    HANDLE Get() const noexcept { return _event; }

    void Destroy() noexcept;

private:
    HANDLE _event{nullptr};

    friend std::optional<Win32Event> MakeWin32Event() noexcept;
};

class VersionedRootSignatureDescContainer {
public:
    class RootConstant {
    public:
        D3D12_ROOT_CONSTANTS data;
        UINT index;
    };

    class RootDescriptor {
    public:
        D3D12_ROOT_DESCRIPTOR1 data;
        D3D12_ROOT_PARAMETER_TYPE type;
        UINT index;
    };

    class DescriptorTable {
    public:
        D3D12_ROOT_DESCRIPTOR_TABLE1 data;
        UINT index;
    };

    VersionedRootSignatureDescContainer() noexcept = default;
    explicit VersionedRootSignatureDescContainer(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc) noexcept;
    VersionedRootSignatureDescContainer(const VersionedRootSignatureDescContainer& other) noexcept;
    VersionedRootSignatureDescContainer(VersionedRootSignatureDescContainer&& other) noexcept;
    VersionedRootSignatureDescContainer& operator=(const VersionedRootSignatureDescContainer& other) noexcept;
    VersionedRootSignatureDescContainer& operator=(VersionedRootSignatureDescContainer&& other) noexcept;
    ~VersionedRootSignatureDescContainer() noexcept = default;

    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* Get() const noexcept { return &_desc; }
    RootConstant GetRootConstant() const noexcept;
    RootDescriptor GetRootDescriptor(uint32_t slot) const noexcept;
    DescriptorTable GetDescriptorTable(uint32_t slot) const noexcept;

    UINT GetDescriptorTableCount() const noexcept;

    friend void swap(VersionedRootSignatureDescContainer& lhs, VersionedRootSignatureDescContainer& rhs) noexcept;

private:
    void RefreshDesc() noexcept;
    void RefreshRanges() noexcept;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC _desc{};
    vector<D3D12_DESCRIPTOR_RANGE> _ranges;
    vector<D3D12_DESCRIPTOR_RANGE1> _ranges1;
    vector<D3D12_ROOT_PARAMETER> _params;
    vector<D3D12_ROOT_PARAMETER1> _params1;
    vector<D3D12_STATIC_SAMPLER_DESC> _staticSamplerDescs;
    vector<D3D12_STATIC_SAMPLER_DESC1> _staticSamplerDescs1;
};

class RootDescriptorTable1Container {
public:
    RootDescriptorTable1Container() noexcept = default;
    explicit RootDescriptorTable1Container(const D3D12_ROOT_DESCRIPTOR_TABLE1& table) noexcept;
    RootDescriptorTable1Container(const RootDescriptorTable1Container& other) noexcept;
    RootDescriptorTable1Container(RootDescriptorTable1Container&& other) noexcept;
    RootDescriptorTable1Container& operator=(const RootDescriptorTable1Container& other) noexcept;
    RootDescriptorTable1Container& operator=(RootDescriptorTable1Container&& other) noexcept;
    ~RootDescriptorTable1Container() noexcept = default;

    const D3D12_ROOT_DESCRIPTOR_TABLE1* Get() const noexcept { return &_table; }
    const vector<D3D12_DESCRIPTOR_RANGE1>& GetRanges() const noexcept { return _ranges; }

    friend void swap(RootDescriptorTable1Container& lhs, RootDescriptorTable1Container& rhs) noexcept;

private:
    void Refresh() noexcept;

    D3D12_ROOT_DESCRIPTOR_TABLE1 _table{};
    vector<D3D12_DESCRIPTOR_RANGE1> _ranges;
};

std::optional<Win32Event> MakeWin32Event() noexcept;

std::string_view GetErrorName(HRESULT hr) noexcept;

void SetObjectName(std::string_view str, ID3D12Object* obj, D3D12MA::Allocation* alloc = nullptr) noexcept;

bool IsStencilFormatDXGI(DXGI_FORMAT fmt) noexcept;

DXGI_FORMAT FormatToTypeless(DXGI_FORMAT fmt) noexcept;

DXGI_FORMAT MapShaderResourceType(TextureFormat v) noexcept;

D3D12_COMMAND_LIST_TYPE MapType(QueueType v) noexcept;
DXGI_FORMAT MapType(TextureFormat v) noexcept;
D3D12_HEAP_TYPE MapType(MemoryType v) noexcept;
D3D12_RESOURCE_DIMENSION MapType(TextureDimension v) noexcept;
D3D12_SHADER_VISIBILITY MapShaderStages(ShaderStages v) noexcept;
struct MapPrimitiveTopologyResult {
    D3D12_PRIMITIVE_TOPOLOGY_TYPE type;
    D3D12_PRIMITIVE_TOPOLOGY topology;
};
std::pair<D3D12_PRIMITIVE_TOPOLOGY_TYPE, D3D12_PRIMITIVE_TOPOLOGY> MapType(PrimitiveTopology v) noexcept;
D3D12_INPUT_CLASSIFICATION MapType(VertexStepMode v) noexcept;
DXGI_FORMAT MapType(VertexFormat v) noexcept;
std::optional<D3D12_FILL_MODE> MapType(PolygonMode v) noexcept;
D3D12_CULL_MODE MapType(CullMode v) noexcept;
D3D12_BLEND_OP MapType(BlendOperation v) noexcept;
D3D12_BLEND MapBlendColor(BlendFactor v) noexcept;
D3D12_BLEND MapBlendAlpha(BlendFactor v) noexcept;
std::optional<D3D12_COLOR_WRITE_ENABLE> MapColorWrites(ColorWrites v) noexcept;
D3D12_COMPARISON_FUNC MapType(CompareFunction v) noexcept;
D3D12_STENCIL_OP MapType(StencilOperation v) noexcept;
D3D12_INDEX_BUFFER_STRIP_CUT_VALUE MapType(IndexFormat v) noexcept;
D3D12_RESOURCE_STATES MapType(BufferUses v) noexcept;
D3D12_RESOURCE_STATES MapType(TextureUses v) noexcept;
D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE MapType(LoadAction v) noexcept;
D3D12_RENDER_PASS_ENDING_ACCESS_TYPE MapType(StoreAction v) noexcept;
D3D12_FILTER_TYPE MapType(FilterMode v) noexcept;
D3D12_FILTER MapType(FilterMode mig, FilterMode mag, FilterMode mipmap, bool hasCompare, uint32_t aniso) noexcept;
D3D12_TEXTURE_ADDRESS_MODE MapType(AddressMode v) noexcept;
D3D12_RESOURCE_STATES MapMemoryTypeToResourceState(MemoryType v) noexcept;

}  // namespace radray::render::d3d12

std::string_view format_as(D3D_FEATURE_LEVEL v) noexcept;
std::string_view format_as(D3D_SHADER_MODEL v) noexcept;
std::string_view format_as(D3D12_RESOURCE_HEAP_TIER v) noexcept;
std::string_view format_as(D3D12_RESOURCE_BINDING_TIER v) noexcept;
std::string_view format_as(D3D12_DESCRIPTOR_HEAP_TYPE v) noexcept;
std::string_view format_as(D3D_ROOT_SIGNATURE_VERSION v) noexcept;
std::string_view format_as(D3D12_ROOT_PARAMETER_TYPE v) noexcept;

#endif
