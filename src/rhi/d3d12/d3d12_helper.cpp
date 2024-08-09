#include "d3d12_helper.h"

#include <vector>

#include <radray/logger.h>

namespace radray::rhi::d3d12 {

const char* GetErrorName(HRESULT hr) noexcept {
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
        default: return "Unknown Error";
    }
}

std::wstring Utf8ToWString(const std::string& str) noexcept {
    if (str.length() >= std::numeric_limits<int>::max()) {
        RADRAY_ABORT("too large string {}", str.length());
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), nullptr, 0);
    std::vector<wchar_t> buffer{};
    buffer.resize(len);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), buffer.data(), (int)buffer.size());
    return std::wstring{buffer.begin(), buffer.end()};
}

std::string Utf8ToString(const std::wstring& str) noexcept {
    if (str.length() >= std::numeric_limits<int>::max()) {
        RADRAY_ABORT("too large string {}", str.length());
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, str.data(), (int)str.length(), nullptr, 0, nullptr, nullptr);
    std::vector<char> buffer{};
    buffer.resize(len);
    WideCharToMultiByte(CP_UTF8, 0, str.data(), (int)str.length(), buffer.data(), (int)buffer.size(), nullptr, nullptr);
    return std::string{buffer.begin(), buffer.end()};
}

D3D12_COMMAND_LIST_TYPE EnumConvert(RadrayQueueType type) noexcept {
    switch (type) {
        case RADRAY_QUEUE_TYPE_DIRECT: return D3D12_COMMAND_LIST_TYPE_DIRECT;
        case RADRAY_QUEUE_TYPE_COMPUTE: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
        case RADRAY_QUEUE_TYPE_COPY: return D3D12_COMMAND_LIST_TYPE_COPY;
        default: return D3D12_COMMAND_LIST_TYPE_NONE;
    }
}

DXGI_FORMAT EnumConvert(RadrayFormat format) noexcept {
    switch (format) {
        case RADRAY_FORMAT_UNKNOWN: return DXGI_FORMAT_UNKNOWN;
        case RADRAY_FORMAT_R8_SINT: return DXGI_FORMAT_R8_SINT;
        case RADRAY_FORMAT_R8_UINT: return DXGI_FORMAT_R8_UINT;
        case RADRAY_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_UNORM;
        case RADRAY_FORMAT_RG8_SINT: return DXGI_FORMAT_R8G8_SINT;
        case RADRAY_FORMAT_RG8_UINT: return DXGI_FORMAT_R8G8_UINT;
        case RADRAY_FORMAT_RG8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
        case RADRAY_FORMAT_RGBA8_SINT: return DXGI_FORMAT_R8G8B8A8_SINT;
        case RADRAY_FORMAT_RGBA8_UINT: return DXGI_FORMAT_R8G8B8A8_UINT;
        case RADRAY_FORMAT_RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case RADRAY_FORMAT_R16_SINT: return DXGI_FORMAT_R16_SINT;
        case RADRAY_FORMAT_R16_UINT: return DXGI_FORMAT_R16_UINT;
        case RADRAY_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case RADRAY_FORMAT_RG16_SINT: return DXGI_FORMAT_R16G16_SINT;
        case RADRAY_FORMAT_RG16_UINT: return DXGI_FORMAT_R16G16_UINT;
        case RADRAY_FORMAT_RG16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
        case RADRAY_FORMAT_RGBA16_SINT: return DXGI_FORMAT_R16G16B16A16_SINT;
        case RADRAY_FORMAT_RGBA16_UINT: return DXGI_FORMAT_R16G16B16A16_UINT;
        case RADRAY_FORMAT_RGBA16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case RADRAY_FORMAT_R32_SINT: return DXGI_FORMAT_R32_SINT;
        case RADRAY_FORMAT_R32_UINT: return DXGI_FORMAT_R32_UINT;
        case RADRAY_FORMAT_RG32_SINT: return DXGI_FORMAT_R32G32_SINT;
        case RADRAY_FORMAT_RG32_UINT: return DXGI_FORMAT_R32G32_UINT;
        case RADRAY_FORMAT_RGBA32_SINT: return DXGI_FORMAT_R32G32B32A32_SINT;
        case RADRAY_FORMAT_RGBA32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
        case RADRAY_FORMAT_R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
        case RADRAY_FORMAT_RG16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
        case RADRAY_FORMAT_RGBA16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case RADRAY_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case RADRAY_FORMAT_RG32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
        case RADRAY_FORMAT_RGBA32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case RADRAY_FORMAT_R10G10B10A2_UINT: return DXGI_FORMAT_R10G10B10A2_UINT;
        case RADRAY_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case RADRAY_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
        case RADRAY_FORMAT_D16_UNORM: return DXGI_FORMAT_D16_UNORM;
        case RADRAY_FORMAT_D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
        case RADRAY_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case RADRAY_FORMAT_D32_FLOAT_S8_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

D3D12_RESOURCE_STATES EnumConvert(RadrayResourceStates state) noexcept {
    if (state == RADRAY_RESOURCE_STATE_GENERIC_READ) {
        return D3D12_RESOURCE_STATE_GENERIC_READ;
    }
    if (state == RADRAY_RESOURCE_STATE_COMMON) {
        return D3D12_RESOURCE_STATE_COMMON;
    }
    if (state == RADRAY_RESOURCE_STATE_PRESENT) {
        return D3D12_RESOURCE_STATE_PRESENT;
    }
    D3D12_RESOURCE_STATES result = D3D12_RESOURCE_STATE_COMMON;
    if (state & RADRAY_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) {
        result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    }
    if (state & RADRAY_RESOURCE_STATE_INDEX_BUFFER) {
        result |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    }
    if (state & RADRAY_RESOURCE_STATE_RENDER_TARGET) {
        result |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (state & RADRAY_RESOURCE_STATE_UNORDERED_ACCESS) {
        result |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (state & RADRAY_RESOURCE_STATE_DEPTH_WRITE) {
        result |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    if (state & RADRAY_RESOURCE_STATE_DEPTH_READ) {
        result |= D3D12_RESOURCE_STATE_DEPTH_READ;
    }
    if (state & RADRAY_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        result |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    if (state & RADRAY_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        result |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    if (state & RADRAY_RESOURCE_STATE_STREAM_OUT) {
        result |= D3D12_RESOURCE_STATE_STREAM_OUT;
    }
    if (state & RADRAY_RESOURCE_STATE_INDIRECT_ARGUMENT) {
        result |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    }
    if (state & RADRAY_RESOURCE_STATE_COPY_DEST) {
        result |= D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if (state & RADRAY_RESOURCE_STATE_COPY_SOURCE) {
        result |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    return result;
}

D3D12_HEAP_TYPE EnumConvert(RadrayHeapUsage usage) noexcept {
    switch (usage) {
        case RADRAY_HEAP_USAGE_DEFAULT: return D3D12_HEAP_TYPE_DEFAULT;
        case RADRAY_HEAP_USAGE_UPLOAD: return D3D12_HEAP_TYPE_UPLOAD;
        case RADRAY_HEAP_USAGE_READBACK: return D3D12_HEAP_TYPE_READBACK;
        default: return (D3D12_HEAP_TYPE)-1;
    }
}

std::string_view to_string(D3D12_DESCRIPTOR_HEAP_TYPE v) noexcept {
    switch (v) {
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: return "CBV_SRV_UAV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: return "SAMPLER";
        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: return "RTV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: return "DSV";
        default: return "UNKNOWN";
    }
}

}  // namespace radray::rhi::d3d12
