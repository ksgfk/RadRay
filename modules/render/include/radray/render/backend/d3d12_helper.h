#pragma once

#ifdef RADRAY_ENABLE_D3D12

#include <optional>

#include <radray/types.h>
#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/render/common.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINDOWS
#define _WINDOWS
#endif
#ifndef UNICODE
#define UNICODE
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
    struct DescriptorRange {
        D3D12_DESCRIPTOR_RANGE_TYPE RangeType{};
        UINT NumDescriptors{};
        UINT BaseShaderRegister{};
        UINT RegisterSpace{};
        UINT OffsetInDescriptorsFromTableStart{};
        D3D12_DESCRIPTOR_RANGE_FLAGS Flags{D3D12_DESCRIPTOR_RANGE_FLAG_NONE};
    };

    struct RootDescriptor {
        UINT ShaderRegister{};
        UINT RegisterSpace{};
        D3D12_ROOT_DESCRIPTOR_FLAGS Flags{D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    };

    struct RootConstants {
        UINT ShaderRegister{};
        UINT RegisterSpace{};
        UINT Num32BitValues{};
    };

    struct RootParameter {
        D3D12_ROOT_PARAMETER_TYPE Type{};
        D3D12_SHADER_VISIBILITY ShaderVisibility{D3D12_SHADER_VISIBILITY_ALL};
        RootConstants Constants{};
        RootDescriptor Descriptor{};
        vector<DescriptorRange> Ranges;
    };

    struct StaticSampler {
        D3D12_FILTER Filter{};
        D3D12_TEXTURE_ADDRESS_MODE AddressU{};
        D3D12_TEXTURE_ADDRESS_MODE AddressV{};
        D3D12_TEXTURE_ADDRESS_MODE AddressW{};
        FLOAT MipLODBias{};
        UINT MaxAnisotropy{};
        D3D12_COMPARISON_FUNC ComparisonFunc{};
        D3D12_STATIC_BORDER_COLOR BorderColor{};
        FLOAT MinLOD{};
        FLOAT MaxLOD{};
        UINT ShaderRegister{};
        UINT RegisterSpace{};
        D3D12_SHADER_VISIBILITY ShaderVisibility{D3D12_SHADER_VISIBILITY_ALL};
        D3D12_SAMPLER_FLAGS Flags{D3D12_SAMPLER_FLAG_NONE};
    };

    struct RootParamRef {
        size_t Index;
        const RootParameter& Param;
    };

    class View {
    public:
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* Get() const noexcept { return &_desc; }

    private:
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC _desc{};
        vector<D3D12_ROOT_PARAMETER> _params0;
        vector<D3D12_DESCRIPTOR_RANGE> _ranges0;
        vector<D3D12_STATIC_SAMPLER_DESC> _samplers0;
        vector<D3D12_ROOT_PARAMETER1> _params1;
        vector<D3D12_DESCRIPTOR_RANGE1> _ranges1;
        vector<D3D12_STATIC_SAMPLER_DESC1> _samplers1;

        friend class VersionedRootSignatureDescContainer;
    };

    VersionedRootSignatureDescContainer() noexcept = default;
    explicit VersionedRootSignatureDescContainer(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc) noexcept;

    View MakeView(D3D_ROOT_SIGNATURE_VERSION version) const noexcept;
    View MakeView() const noexcept { return MakeView(_sourceVersion); }

    D3D_ROOT_SIGNATURE_VERSION GetSourceVersion() const noexcept { return _sourceVersion; }
    D3D12_ROOT_SIGNATURE_FLAGS GetFlags() const noexcept { return _flags; }
    std::span<const RootParameter> GetParameters() const noexcept { return _parameters; }
    std::span<const StaticSampler> GetStaticSamplers() const noexcept { return _staticSamplers; }

    RootParamRef GetTable(size_t index) const noexcept;
    size_t GetTableCount() const noexcept { return _tableOffsets.size(); }
    RootParamRef GetRootConstant(size_t index) const noexcept;
    size_t GetRootConstantCount() const noexcept { return _rootConstantsOffsets.size(); }
    RootParamRef GetRootDescriptor(size_t index) const noexcept;
    size_t GetRootDescriptorCount() const noexcept { return _rootDescriptorsOffsets.size(); }

private:
    D3D_ROOT_SIGNATURE_VERSION _sourceVersion{D3D_ROOT_SIGNATURE_VERSION_1};
    D3D12_ROOT_SIGNATURE_FLAGS _flags{D3D12_ROOT_SIGNATURE_FLAG_NONE};
    vector<RootParameter> _parameters;
    vector<StaticSampler> _staticSamplers;

    vector<size_t> _tableOffsets;
    vector<size_t> _rootConstantsOffsets;
    vector<size_t> _rootDescriptorsOffsets;
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
MapPrimitiveTopologyResult MapType(PrimitiveTopology v) noexcept;
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
