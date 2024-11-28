#include "d3d12_helper.h"

#include <type_traits>

namespace radray::render::d3d12 {

constexpr size_t MaxNameLength = 128;

std::string_view GetErrorName(HRESULT hr) noexcept {
    switch (hr) {
        case D3D12_ERROR_ADAPTER_NOT_FOUND: return "D3D12_ERROR_ADAPTER_NOT_FOUND";
        case D3D12_ERROR_DRIVER_VERSION_MISMATCH: return "D3D12_ERROR_DRIVER_VERSION_MISMATCH";
        case DXGI_ERROR_ACCESS_DENIED: return "DXGI_ERROR_ACCESS_DENIED";
        case DXGI_ERROR_ACCESS_LOST: return "DXGI_ERROR_ACCESS_LOST";
        case DXGI_ERROR_ALREADY_EXISTS: return "DXGI_ERROR_ALREADY_EXISTS";
        case DXGI_ERROR_CANNOT_PROTECT_CONTENT: return "DXGI_ERROR_CANNOT_PROTECT_CONTENT";
        case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
        case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
        case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case DXGI_ERROR_GRAPHICS_VIDPN_SOURCE_IN_USE: return "DXGI_ERROR_GRAPHICS_VIDPN_SOURCE_IN_USE";
        case DXGI_ERROR_FRAME_STATISTICS_DISJOINT: return "DXGI_ERROR_FRAME_STATISTICS_DISJOINT";
        case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
        case DXGI_ERROR_MORE_DATA: return "DXGI_ERROR_MORE_DATA";
        case DXGI_ERROR_NAME_ALREADY_EXISTS: return "DXGI_ERROR_NAME_ALREADY_EXISTS";
        case DXGI_ERROR_NONEXCLUSIVE: return "DXGI_ERROR_NONEXCLUSIVE";
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
        case DXGI_ERROR_NOT_FOUND: return "DXGI_ERROR_NOT_FOUND";
        case DXGI_ERROR_REMOTE_CLIENT_DISCONNECTED: return "DXGI_ERROR_REMOTE_CLIENT_DISCONNECTED";
        case DXGI_ERROR_REMOTE_OUTOFMEMORY: return "DXGI_ERROR_REMOTE_OUTOFMEMORY";
        case DXGI_ERROR_RESTRICT_TO_OUTPUT_STALE: return "DXGI_ERROR_RESTRICT_TO_OUTPUT_STALE";
        case DXGI_ERROR_SDK_COMPONENT_MISSING: return "DXGI_ERROR_SDK_COMPONENT_MISSING";
        case DXGI_ERROR_SESSION_DISCONNECTED: return "DXGI_ERROR_SESSION_DISCONNECTED";
        case DXGI_ERROR_UNSUPPORTED: return "DXGI_ERROR_UNSUPPORTED";
        case DXGI_ERROR_WAIT_TIMEOUT: return "DXGI_ERROR_WAIT_TIMEOUT";
        case DXGI_ERROR_WAS_STILL_DRAWING: return "DXGI_ERROR_WAS_STILL_DRAWING";
        case E_FAIL: return "E_FAIL";
        case E_INVALIDARG: return "E_INVALIDARG";
        case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
        case E_NOTIMPL: return "E_NOTIMPL";
        case S_FALSE: return "S_FALSE";
        case S_OK: return "S_OK";
        default: return "UNKNOWN";
    }
}

void SetObjectName(std::string_view str, ID3D12Object* obj, D3D12MA::Allocation* alloc) noexcept {
    wchar_t debugName[MaxNameLength]{};
    MultiByteToWideChar(CP_UTF8, 0, str.data(), str.length(), debugName, MaxNameLength);
    if (alloc) {
        alloc->SetName(debugName);
    }
    obj->SetName(debugName);
}

UINT SubresourceIndex(UINT MipSlice, UINT ArraySlice, UINT PlaneSlice, UINT MipLevels, UINT ArraySize) noexcept {
    return ((MipSlice) + ((ArraySlice) * (MipLevels)) + ((PlaneSlice) * (MipLevels) * (ArraySize)));
}

D3D12_COMMAND_LIST_TYPE MapType(QueueType v) noexcept {
    switch (v) {
        case QueueType::Direct: return D3D12_COMMAND_LIST_TYPE_DIRECT;
        case QueueType::Compute: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
        case QueueType::Copy: return D3D12_COMMAND_LIST_TYPE_COPY;
    }
}

D3D12_SHADER_VISIBILITY MapType(ShaderStage v) noexcept {
    switch (v) {
        case ShaderStage::UNKNOWN: return D3D12_SHADER_VISIBILITY_ALL;
        case ShaderStage::Vertex: return D3D12_SHADER_VISIBILITY_VERTEX;
        case ShaderStage::Pixel: return D3D12_SHADER_VISIBILITY_PIXEL;
        case ShaderStage::Compute: return D3D12_SHADER_VISIBILITY_ALL;
    }
}

D3D12_SHADER_VISIBILITY MapType(ShaderStages v) noexcept {
    if (v == ShaderStage::Compute) {
        return D3D12_SHADER_VISIBILITY_ALL;
    }
    if (v == ShaderStage::UNKNOWN) {
        return D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_SHADER_VISIBILITY res = D3D12_SHADER_VISIBILITY_ALL;
    uint32_t stageCount = 0;
    if (HasFlag(v, ShaderStage::Vertex)) {
        res = D3D12_SHADER_VISIBILITY_VERTEX;
        ++stageCount;
    }
    if (HasFlag(v, ShaderStage::Pixel)) {
        res = D3D12_SHADER_VISIBILITY_PIXEL;
        ++stageCount;
    }
    return stageCount > 1 ? D3D12_SHADER_VISIBILITY_ALL : res;
}

D3D12_DESCRIPTOR_RANGE_TYPE MapDescRangeType(ShaderResourceType v) noexcept {
    switch (v) {
        case ShaderResourceType::CBuffer: return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        case ShaderResourceType::Texture: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        case ShaderResourceType::Buffer: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        case ShaderResourceType::RWTexture: return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        case ShaderResourceType::RWBuffer: return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        case ShaderResourceType::Sampler: return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        case ShaderResourceType::PushConstant: return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        case ShaderResourceType::RayTracing: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    }
}

D3D12_FILTER_TYPE MapType(FilterMode v) noexcept {
    switch (v) {
        case FilterMode::Nearest: return D3D12_FILTER_TYPE_POINT;
        case FilterMode::Linear: return D3D12_FILTER_TYPE_LINEAR;
    }
}

D3D12_FILTER MapType(FilterMode mig, FilterMode mag, FilterMode mipmap, bool hasCompare, uint32_t aniso) noexcept {
    D3D12_FILTER_TYPE minFilter = MapType(mig);
    D3D12_FILTER_TYPE magFilter = MapType(mag);
    D3D12_FILTER_TYPE mipmapFilter = MapType(mipmap);
    D3D12_FILTER_REDUCTION_TYPE reduction = hasCompare ? D3D12_FILTER_REDUCTION_TYPE_COMPARISON : D3D12_FILTER_REDUCTION_TYPE_STANDARD;
    if (aniso > 1) {
        if (mipmapFilter == D3D12_FILTER_TYPE_POINT) {
            return D3D12_ENCODE_MIN_MAG_ANISOTROPIC_MIP_POINT_FILTER(reduction);
        } else {
            return D3D12_ENCODE_ANISOTROPIC_FILTER(reduction);
        }
    } else {
        return D3D12_ENCODE_BASIC_FILTER(minFilter, magFilter, mipmapFilter, reduction);
    }
}

D3D12_TEXTURE_ADDRESS_MODE MapType(AddressMode v) noexcept {
    switch (v) {
        case AddressMode::ClampToEdge: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case AddressMode::Repeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case AddressMode::Mirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    }
}

D3D12_COMPARISON_FUNC MapType(CompareFunction v) noexcept {
    switch (v) {
        case CompareFunction::Never: return D3D12_COMPARISON_FUNC_NEVER;
        case CompareFunction::Less: return D3D12_COMPARISON_FUNC_LESS;
        case CompareFunction::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
        case CompareFunction::LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case CompareFunction::Greater: return D3D12_COMPARISON_FUNC_GREATER;
        case CompareFunction::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case CompareFunction::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case CompareFunction::Always: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

}  // namespace radray::render::d3d12

std::string_view format_as(D3D_FEATURE_LEVEL v) noexcept {
    switch (v) {
        case D3D_FEATURE_LEVEL_11_0: return "11.0";
        case D3D_FEATURE_LEVEL_11_1: return "11.1";
        case D3D_FEATURE_LEVEL_12_0: return "12.0";
        case D3D_FEATURE_LEVEL_12_1: return "12.1";
        case D3D_FEATURE_LEVEL_12_2: return "12.2";
        default: return "UNKNOWN";
    }
}
std::string_view format_as(D3D_SHADER_MODEL v) noexcept {
    switch (v) {
        case D3D_SHADER_MODEL_5_1: return "5.1";
        case D3D_SHADER_MODEL_6_0: return "6.0";
        case D3D_SHADER_MODEL_6_1: return "6.1";
        case D3D_SHADER_MODEL_6_2: return "6.2";
        case D3D_SHADER_MODEL_6_3: return "6.3";
        case D3D_SHADER_MODEL_6_4: return "6.4";
        case D3D_SHADER_MODEL_6_5: return "6.5";
        case D3D_SHADER_MODEL_6_6: return "6.6";
        case D3D_SHADER_MODEL_6_7: return "6.7";
        case D3D_SHADER_MODEL_6_8: return "6.8";
        case D3D_SHADER_MODEL_6_9: return "6.9";
        default: return "UNKNOWN";
    }
}
std::string_view format_as(D3D12_RESOURCE_HEAP_TIER v) noexcept {
    switch (v) {
        case D3D12_RESOURCE_HEAP_TIER_1: return "1";
        case D3D12_RESOURCE_HEAP_TIER_2: return "2";
    }
}
std::string_view format_as(D3D12_RESOURCE_BINDING_TIER v) noexcept {
    switch (v) {
        case D3D12_RESOURCE_BINDING_TIER_1: return "1";
        case D3D12_RESOURCE_BINDING_TIER_2: return "2";
        case D3D12_RESOURCE_BINDING_TIER_3: return "3";
    }
}
