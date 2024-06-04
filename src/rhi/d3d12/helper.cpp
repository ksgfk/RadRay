#include "helper.h"

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

uint32_t DxgiFormatByteSize(DXGI_FORMAT format) noexcept {
    switch (format) {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return 16;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
        case DXGI_FORMAT_R32G32B32A32_UINT: return 16;
        case DXGI_FORMAT_R32G32B32A32_SINT: return 16;
        case DXGI_FORMAT_R32G32B32_TYPELESS: return 12;
        case DXGI_FORMAT_R32G32B32_FLOAT: return 12;
        case DXGI_FORMAT_R32G32B32_UINT: return 12;
        case DXGI_FORMAT_R32G32B32_SINT: return 12;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return 8;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
        case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
        case DXGI_FORMAT_R16G16B16A16_UINT: return 8;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return 8;
        case DXGI_FORMAT_R16G16B16A16_SINT: return 8;
        case DXGI_FORMAT_R32G32_TYPELESS: return 8;
        case DXGI_FORMAT_R32G32_FLOAT: return 8;
        case DXGI_FORMAT_R32G32_UINT: return 8;
        case DXGI_FORMAT_R32G32_SINT: return 8;
        case DXGI_FORMAT_R32G8X24_TYPELESS: return 8;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return 8;
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return 8;
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return 8;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return 4;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
        case DXGI_FORMAT_R10G10B10A2_UINT: return 4;
        case DXGI_FORMAT_R11G11B10_FLOAT: return 4;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return 4;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return 4;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 4;
        case DXGI_FORMAT_R8G8B8A8_UINT: return 4;
        case DXGI_FORMAT_R8G8B8A8_SNORM: return 4;
        case DXGI_FORMAT_R8G8B8A8_SINT: return 4;
        case DXGI_FORMAT_R16G16_TYPELESS: return 4;
        case DXGI_FORMAT_R16G16_FLOAT: return 4;
        case DXGI_FORMAT_R16G16_UNORM: return 4;
        case DXGI_FORMAT_R16G16_UINT: return 4;
        case DXGI_FORMAT_R16G16_SNORM: return 4;
        case DXGI_FORMAT_R16G16_SINT: return 4;
        case DXGI_FORMAT_R32_TYPELESS: return 4;
        case DXGI_FORMAT_D32_FLOAT: return 4;
        case DXGI_FORMAT_R32_FLOAT: return 4;
        case DXGI_FORMAT_R32_UINT: return 4;
        case DXGI_FORMAT_R32_SINT: return 4;
        case DXGI_FORMAT_R24G8_TYPELESS: return 4;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return 4;
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return 4;
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return 4;
        case DXGI_FORMAT_R8G8_TYPELESS: return 2;
        case DXGI_FORMAT_R8G8_UNORM: return 2;
        case DXGI_FORMAT_R8G8_UINT: return 2;
        case DXGI_FORMAT_R8G8_SNORM: return 2;
        case DXGI_FORMAT_R8G8_SINT: return 2;
        case DXGI_FORMAT_R16_TYPELESS: return 2;
        case DXGI_FORMAT_R16_FLOAT: return 2;
        case DXGI_FORMAT_D16_UNORM: return 2;
        case DXGI_FORMAT_R16_UNORM: return 2;
        case DXGI_FORMAT_R16_UINT: return 2;
        case DXGI_FORMAT_R16_SNORM: return 2;
        case DXGI_FORMAT_R16_SINT: return 2;
        case DXGI_FORMAT_R8_TYPELESS: return 1;
        case DXGI_FORMAT_R8_UNORM: return 1;
        case DXGI_FORMAT_R8_UINT: return 1;
        case DXGI_FORMAT_R8_SNORM: return 1;
        case DXGI_FORMAT_R8_SINT: return 1;
        default: return -1;
    }
}

D3D12_COMMAND_LIST_TYPE ToCmdListType(CommandListType type) noexcept {
    switch (type) {
        case CommandListType::Graphics: return D3D12_COMMAND_LIST_TYPE_DIRECT;
        case CommandListType::Compute: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    }
}

D3D12_HEAP_TYPE ToHeapType(BufferType type) noexcept {
    switch (type) {
        case BufferType::Default: return D3D12_HEAP_TYPE_DEFAULT;
        case BufferType::Upload: return D3D12_HEAP_TYPE_UPLOAD;
        case BufferType::Readback: return D3D12_HEAP_TYPE_READBACK;
    }
}

DXGI_FORMAT ToDxgiFormat(RhiFormat format) noexcept {
    switch (format) {
        case RhiFormat::Unknown:
        case RhiFormat::R32G32B32A32_Typeless:
        case RhiFormat::R32G32B32A32_Float:
        case RhiFormat::R32G32B32A32_UInt:
        case RhiFormat::R32G32B32A32_SInt:
        case RhiFormat::R32G32B32_Typeless:
        case RhiFormat::R32G32B32_Float:
        case RhiFormat::R32G32B32_UInt:
        case RhiFormat::R32G32B32_SInt:
        case RhiFormat::R16G16B16A16_Typeless:
        case RhiFormat::R16G16B16A16_Float:
        case RhiFormat::R16G16B16A16_UNorm:
        case RhiFormat::R16G16B16A16_UInt:
        case RhiFormat::R16G16B16A16_SNorm:
        case RhiFormat::R16G16B16A16_SInt:
        case RhiFormat::R32G32_Typeless:
        case RhiFormat::R32G32_Float:
        case RhiFormat::R32G32_UInt:
        case RhiFormat::R32G32_SInt:
        case RhiFormat::R32G8X24_Typeless:
        case RhiFormat::D32_Float_S8X24_UInt:
        case RhiFormat::R32_Float_X8X24_Typeless:
        case RhiFormat::X32_Typeless_G8X24_UInt:
        case RhiFormat::R10G10B10A2_Typeless:
        case RhiFormat::R10G10B10A2_UNorm:
        case RhiFormat::R10G10B10A2_UInt:
        case RhiFormat::R11G11B10_Float:
        case RhiFormat::R8G8B8A8_Typeless:
        case RhiFormat::R8G8B8A8_UNorm:
        case RhiFormat::R8G8B8A8_UNorm_SRGB:
        case RhiFormat::R8G8B8A8_UInt:
        case RhiFormat::R8G8B8A8_SNorm:
        case RhiFormat::R8G8B8A8_SInt:
        case RhiFormat::R16G16_Typeless:
        case RhiFormat::R16G16_Float:
        case RhiFormat::R16G16_UNorm:
        case RhiFormat::R16G16_UInt:
        case RhiFormat::R16G16_SNorm:
        case RhiFormat::R16G16_SInt:
        case RhiFormat::R32_Typeless:
        case RhiFormat::D32_Float:
        case RhiFormat::R32_Float:
        case RhiFormat::R32_UInt:
        case RhiFormat::R32_SInt:
        case RhiFormat::R24G8_Typeless:
        case RhiFormat::D24_UNorm_S8_UInt:
        case RhiFormat::R24_UNorm_X8_Typeless:
        case RhiFormat::X24_Typeless_G8_UInt:
        case RhiFormat::R8G8_Typeless:
        case RhiFormat::R8G8_UNorm:
        case RhiFormat::R8G8_UInt:
        case RhiFormat::R8G8_SNorm:
        case RhiFormat::R8G8_SInt:
        case RhiFormat::R16_Typeless:
        case RhiFormat::R16_Float:
        case RhiFormat::D16_UNorm:
        case RhiFormat::R16_UNorm:
        case RhiFormat::R16_UInt:
        case RhiFormat::R16_SNorm:
        case RhiFormat::R16_SInt:
        case RhiFormat::R8_Typeless:
        case RhiFormat::R8_UNorm:
        case RhiFormat::R8_UInt:
        case RhiFormat::R8_SNorm:
        case RhiFormat::R8_SInt:
        case RhiFormat::A8_UNorm:
        case RhiFormat::R1_UNorm:
        case RhiFormat::R9G9B9E5_SharedExp:
        case RhiFormat::R8G8_B8G8_UNorm:
        case RhiFormat::G8R8_G8B8_UNorm:
        case RhiFormat::BC1_Typeless:
        case RhiFormat::BC1_UNorm:
        case RhiFormat::BC1_UNorm_SRGB:
        case RhiFormat::BC2_Typeless:
        case RhiFormat::BC2_UNorm:
        case RhiFormat::BC2_UNorm_SRGB:
        case RhiFormat::BC3_Typeless:
        case RhiFormat::BC3_UNorm:
        case RhiFormat::BC3_UNorm_SRGB:
        case RhiFormat::BC4_Typeless:
        case RhiFormat::BC4_UNorm:
        case RhiFormat::BC4_SNorm:
        case RhiFormat::BC5_Typeless:
        case RhiFormat::BC5_UNorm:
        case RhiFormat::BC5_SNorm:
        case RhiFormat::B5G6R5_UNorm:
        case RhiFormat::B5G5R5A1_UNorm:
        case RhiFormat::B8G8R8A8_UNorm:
        case RhiFormat::B8G8R8X8_UNorm:
        case RhiFormat::R10G10B10_XR_BIAS_A2_UNorm:
        case RhiFormat::B8G8R8A8_Typeless:
        case RhiFormat::B8G8R8A8_UNorm_SRGB:
        case RhiFormat::B8G8R8X8_Typeless:
        case RhiFormat::B8G8R8X8_UNorm_SRGB: break;
    }
}

RhiFormat ToRhiFormat(DXGI_FORMAT format) noexcept;

}  // namespace radray::rhi::d3d12
