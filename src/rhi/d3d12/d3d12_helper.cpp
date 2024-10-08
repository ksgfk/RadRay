#include "d3d12_helper.h"

#include <radray/logger.h>

namespace radray::rhi::d3d12 {

constexpr size_t MaxNameLength = 128;

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

radray::wstring Utf8ToWString(const radray::string& str) noexcept {
    if (str.length() >= std::numeric_limits<int>::max()) {
        RADRAY_ABORT("too large string {}", str.length());
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), nullptr, 0);
    radray::vector<wchar_t> buffer{};
    buffer.resize(len);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), buffer.data(), (int)buffer.size());
    return radray::wstring{buffer.begin(), buffer.end()};
}

radray::string Utf8ToString(const radray::wstring& str) noexcept {
    if (str.length() >= std::numeric_limits<int>::max()) {
        RADRAY_ABORT("too large string {}", str.length());
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, str.data(), (int)str.length(), nullptr, 0, nullptr, nullptr);
    radray::vector<char> buffer{};
    buffer.resize(len);
    WideCharToMultiByte(CP_UTF8, 0, str.data(), (int)str.length(), buffer.data(), (int)buffer.size(), nullptr, nullptr);
    return radray::string{buffer.begin(), buffer.end()};
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

D3D12_COMMAND_LIST_TYPE EnumConvert(RadrayQueueType type) noexcept {
    switch (type) {
        case RADRAY_QUEUE_TYPE_DIRECT: return D3D12_COMMAND_LIST_TYPE_DIRECT;
        case RADRAY_QUEUE_TYPE_COMPUTE: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
        case RADRAY_QUEUE_TYPE_COPY: return D3D12_COMMAND_LIST_TYPE_COPY;
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
        case RADRAY_FORMAT_BGRA8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
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
    }
}

D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE EnumConvert(RadrayLoadAction load) noexcept {
    switch (load) {
        case RADRAY_LOAD_ACTION_DONTCARE: return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
        case RADRAY_LOAD_ACTION_LOAD: return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
        case RADRAY_LOAD_ACTION_CLEAR: return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
    }
}

D3D12_RENDER_PASS_ENDING_ACCESS_TYPE EnumConvert(RadrayStoreAction store) noexcept {
    switch (store) {
        case RADRAY_STORE_ACTION_STORE: return D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
        case RADRAY_STORE_ACTION_DISCARD: return D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD;
    }
}

D3D12_TEXTURE_ADDRESS_MODE EnumConvert(RadrayAddressMode addr) noexcept {
    switch (addr) {
        case RADRAY_ADDRESS_MODE_MIRROR: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case RADRAY_ADDRESS_MODE_REPEAT: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case RADRAY_ADDRESS_MODE_CLAMP_TO_EDGE: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case RADRAY_ADDRESS_MODE_CLAMP_TO_BORDER: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
}

D3D12_COMPARISON_FUNC EnumConvert(RadrayCompareMode comp) noexcept {
    switch (comp) {
        case RADRAY_COMPARE_NEVER: return D3D12_COMPARISON_FUNC_NEVER;
        case RADRAY_COMPARE_LESS: return D3D12_COMPARISON_FUNC_LESS;
        case RADRAY_COMPARE_EQUAL: return D3D12_COMPARISON_FUNC_EQUAL;
        case RADRAY_COMPARE_LEQUAL: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case RADRAY_COMPARE_GREATER: return D3D12_COMPARISON_FUNC_GREATER;
        case RADRAY_COMPARE_NOTEQUAL: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case RADRAY_COMPARE_GEQUAL: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case RADRAY_COMPARE_ALWAYS: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
}

DXGI_FORMAT TypelessFormat(DXGI_FORMAT fmt) noexcept {
    switch (fmt) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT: return DXGI_FORMAT_R32G32B32A32_TYPELESS;
        case DXGI_FORMAT_R32G32B32_FLOAT:
        case DXGI_FORMAT_R32G32B32_UINT:
        case DXGI_FORMAT_R32G32B32_SINT: return DXGI_FORMAT_R32G32B32_TYPELESS;

        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;

        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT: return DXGI_FORMAT_R32G32_TYPELESS;

        case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT: return DXGI_FORMAT_R10G10B10A2_TYPELESS;

        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT: return DXGI_FORMAT_R16G16_TYPELESS;

        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT:
        case DXGI_FORMAT_R32_SINT: return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_SINT: return DXGI_FORMAT_R8G8_TYPELESS;
        case DXGI_FORMAT_B4G4R4A4_UNORM:  // just treats a 16 raw bits
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT: return DXGI_FORMAT_R16_TYPELESS;
        case DXGI_FORMAT_A8_UNORM:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT: return DXGI_FORMAT_R8_TYPELESS;
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_TYPELESS;
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_TYPELESS;
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_TYPELESS;
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM: return DXGI_FORMAT_BC4_TYPELESS;
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM: return DXGI_FORMAT_BC5_TYPELESS;
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM: return DXGI_FORMAT_R16_TYPELESS;

        case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;

        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;

        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;

        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16: return DXGI_FORMAT_BC6H_TYPELESS;

        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_TYPELESS;

        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC7_TYPELESS: return fmt;

        case DXGI_FORMAT_R1_UNORM:
        case DXGI_FORMAT_R8G8_B8G8_UNORM:
        case DXGI_FORMAT_G8R8_G8B8_UNORM:
        case DXGI_FORMAT_AYUV:
        case DXGI_FORMAT_Y410:
        case DXGI_FORMAT_Y416:
        case DXGI_FORMAT_NV12:
        case DXGI_FORMAT_P010:
        case DXGI_FORMAT_P016:
        case DXGI_FORMAT_420_OPAQUE:
        case DXGI_FORMAT_YUY2:
        case DXGI_FORMAT_Y210:
        case DXGI_FORMAT_Y216:
        case DXGI_FORMAT_NV11:
        case DXGI_FORMAT_AI44:
        case DXGI_FORMAT_IA44:
        case DXGI_FORMAT_P8:
        case DXGI_FORMAT_A8P8:
        case DXGI_FORMAT_P208:
        case DXGI_FORMAT_V208:
        case DXGI_FORMAT_V408:
        case DXGI_FORMAT_UNKNOWN:
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

D3D12_FILTER ConvertFilter(RadrayFilterMode mig, RadrayFilterMode mag, RadrayMipMapMode mip, bool isAniso, bool isComp) noexcept {
    if (isAniso) {
        return isComp ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC;
    }
    switch (mig) {
        case RADRAY_FILTER_MODE_NEAREST: {  // min POINT
            switch (mag) {
                case RADRAY_FILTER_MODE_NEAREST: {  // mag POINT
                    switch (mip) {
                        case RADRAY_MIPMAP_MODE_NEAREST: {  // mip POINT
                            return isComp ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
                        }
                        case RADRAY_MIPMAP_MODE_LINEAR: {  // mip LINEAR
                            return isComp ? D3D12_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR : D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
                        }
                    }
                }
                case RADRAY_FILTER_MODE_LINEAR: {  // mag LINEAR
                    switch (mip) {
                        case RADRAY_MIPMAP_MODE_NEAREST: {  // mip POINT
                            return isComp ? D3D12_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
                        }
                        case RADRAY_MIPMAP_MODE_LINEAR: {  // mip LINEAR
                            return isComp ? D3D12_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR : D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR;
                        }
                    }
                }
            }
        }
        case RADRAY_FILTER_MODE_LINEAR: {  // min LINEAR
            switch (mag) {
                case RADRAY_FILTER_MODE_NEAREST: {  // mag POINT
                    switch (mip) {
                        case RADRAY_MIPMAP_MODE_NEAREST: {  // mip POINT
                            return isComp ? D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT : D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
                        }
                        case RADRAY_MIPMAP_MODE_LINEAR: {  // mip LINEAR
                            return isComp ? D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR : D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
                        }
                    }
                }
                case RADRAY_FILTER_MODE_LINEAR: {  // mag LINEAR
                    switch (mip) {
                        case RADRAY_MIPMAP_MODE_NEAREST: {  // mip POINT
                            return isComp ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
                        }
                        case RADRAY_MIPMAP_MODE_LINEAR: {  // mip LINEAR
                            return isComp ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                        }
                    }
                }
            }
        }
    }
}

std::string_view to_string(D3D12_DESCRIPTOR_HEAP_TYPE v) noexcept {
    switch (v) {
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: return "CBV_SRV_UAV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: return "SAMPLER";
        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: return "RTV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: return "DSV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES: return "UNKNOWN";
    }
}

}  // namespace radray::rhi::d3d12
