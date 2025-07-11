#pragma once

#include <string_view>

#include <radray/platform.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/render/common.h>
#include <radray/render/resource.h>

#include <windows.h>
#include <wrl.h>
#include <dxgi1_6.h>
#include <directx/d3dx12.h>
#include <directx/d3d12shader.h>
#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED
#include <D3D12MemAlloc.h>

namespace radray::render::d3d12 {

class DeviceD3D12;
class CmdQueueD3D12;
class CmdAllocatorD3D12;
class CmdListD3D12;
class FenceD3D12;
class RootSigD3D12;
class Dxil;
class GraphicsPsoD3D12;
class SwapChainD3D12;
class TextureD3D12;
class BufferD3D12;
class DescriptorHeap;
class SamplerD3D12;

using Microsoft::WRL::ComPtr;

class Win32Event : public Noncopyable {
public:
    Win32Event() noexcept = default;
    ~Win32Event() noexcept;
    Win32Event(Win32Event&& other) noexcept;
    Win32Event& operator=(Win32Event&& other) noexcept;

    HANDLE Get() const noexcept { return _event; }

    void Destroy() noexcept;

private:
    HANDLE _event{nullptr};

    friend std::optional<Win32Event> MakeWin32Event() noexcept;
};

class ResourceViewD3D12 : public ResourceView {
public:
    explicit ResourceViewD3D12(ResourceView::Type type) noexcept : _type(type) {}
    ~ResourceViewD3D12() noexcept override = default;

    ResourceView::Type GetViewType() const noexcept final { return _type; }

public:
    ResourceView::Type _type;
};

std::optional<Win32Event> MakeWin32Event() noexcept;

std::string_view GetErrorName(HRESULT hr) noexcept;
void SetObjectName(std::string_view str, ID3D12Object* obj, D3D12MA::Allocation* alloc = nullptr) noexcept;
UINT SubresourceIndex(UINT MipSlice, UINT ArraySlice, UINT PlaneSlice, UINT MipLevels, UINT ArraySize) noexcept;
DXGI_FORMAT FormatToTypeless(DXGI_FORMAT fmt) noexcept;

D3D12_COMMAND_LIST_TYPE MapType(QueueType v) noexcept;
D3D12_SHADER_VISIBILITY MapType(ShaderStage v) noexcept;
D3D12_SHADER_VISIBILITY MapShaderStages(ShaderStages v) noexcept;
D3D12_DESCRIPTOR_RANGE_TYPE MapDescRangeType(ShaderResourceType v) noexcept;
D3D12_DESCRIPTOR_RANGE_TYPE MapDescRangeType(ResourceType v) noexcept;
D3D12_FILTER_TYPE MapType(FilterMode v) noexcept;
D3D12_FILTER MapType(FilterMode mig, FilterMode mag, FilterMode mipmap, bool hasCompare, uint32_t aniso) noexcept;
D3D12_TEXTURE_ADDRESS_MODE MapType(AddressMode v) noexcept;
D3D12_COMPARISON_FUNC MapType(CompareFunction v) noexcept;
std::pair<D3D12_PRIMITIVE_TOPOLOGY_TYPE, D3D12_PRIMITIVE_TOPOLOGY> MapType(PrimitiveTopology v) noexcept;
D3D12_INPUT_CLASSIFICATION MapType(VertexStepMode v) noexcept;
DXGI_FORMAT MapType(VertexFormat v) noexcept;
DXGI_FORMAT MapType(TextureFormat v) noexcept;
DXGI_FORMAT MapShaderResourceType(TextureFormat v) noexcept;
std::optional<D3D12_FILL_MODE> MapType(PolygonMode v) noexcept;
D3D12_CULL_MODE MapType(CullMode v) noexcept;
D3D12_BLEND_OP MapType(BlendOperation v) noexcept;
D3D12_BLEND MapBlendColor(BlendFactor v) noexcept;
D3D12_BLEND MapBlendAlpha(BlendFactor v) noexcept;
std::optional<D3D12_COLOR_WRITE_ENABLE> MapColorWrites(ColorWrites v) noexcept;
D3D12_STENCIL_OP MapType(StencilOperation v) noexcept;
D3D12_INDEX_BUFFER_STRIP_CUT_VALUE MapType(IndexFormat v) noexcept;
ResourceStates MapType(D3D12_RESOURCE_STATES v) noexcept;
D3D12_RESOURCE_STATES MapTypeResStates(ResourceStates v) noexcept;
D3D12_HEAP_TYPE MapType(ResourceMemoryUsage v) noexcept;
D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE MapType(LoadAction v) noexcept;
D3D12_RENDER_PASS_ENDING_ACCESS_TYPE MapType(StoreAction v) noexcept;

}  // namespace radray::render::d3d12

#ifndef RADRAY_DX_CHECK
#define RADRAY_DX_CHECK(x)                                                                                                  \
    do {                                                                                                                    \
        HRESULT hr_ = (x);                                                                                                  \
        if (FAILED(hr_)) [[unlikely]] {                                                                                     \
            RADRAY_ABORT("D3D12 error '{} with error {} (code = {})", #x, ::radray::render::d3d12::GetErrorName(hr_), hr_); \
        }                                                                                                                   \
    } while (false)
#endif

std::string_view format_as(D3D_FEATURE_LEVEL v) noexcept;
std::string_view format_as(D3D_SHADER_MODEL v) noexcept;
std::string_view format_as(D3D12_RESOURCE_HEAP_TIER v) noexcept;
std::string_view format_as(D3D12_RESOURCE_BINDING_TIER v) noexcept;
std::string_view format_as(D3D12_DESCRIPTOR_HEAP_TYPE v) noexcept;
