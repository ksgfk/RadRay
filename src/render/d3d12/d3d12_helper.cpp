#include "d3d12_helper.h"

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

}  // namespace radray::render::d3d12

std::string_view format_as(D3D12_DESCRIPTOR_HEAP_TYPE v) noexcept {
    switch (v) {
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: return "CBV_SRV_UAV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: return "SAMPLER";
        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: return "RTV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: return "DSV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES: return "UNKNOWN";
    }
}
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
