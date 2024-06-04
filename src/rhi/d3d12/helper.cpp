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

DXGI_FORMAT ToDxgiFormat(Format format) noexcept {
    switch (format) {
        case Format::Unknown: return DXGI_FORMAT_UNKNOWN;
        case Format::R32G32B32A32_Typeless: return DXGI_FORMAT_R32G32B32A32_TYPELESS;
        case Format::R32G32B32A32_Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::R32G32B32A32_UInt: return DXGI_FORMAT_R32G32B32A32_UINT;
        case Format::R32G32B32A32_SInt: return DXGI_FORMAT_R32G32B32A32_SINT;
        case Format::R32G32B32_Typeless: return DXGI_FORMAT_R32G32B32_TYPELESS;
        case Format::R32G32B32_Float: return DXGI_FORMAT_R32G32B32_FLOAT;
        case Format::R32G32B32_UInt: return DXGI_FORMAT_R32G32B32_UINT;
        case Format::R32G32B32_SInt: return DXGI_FORMAT_R32G32B32_SINT;
        case Format::R16G16B16A16_Typeless: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        case Format::R16G16B16A16_Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::R16G16B16A16_UNorm: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case Format::R16G16B16A16_UInt: return DXGI_FORMAT_R16G16B16A16_UINT;
        case Format::R16G16B16A16_SNorm: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case Format::R16G16B16A16_SInt: return DXGI_FORMAT_R16G16B16A16_SINT;
        case Format::R32G32_Typeless: return DXGI_FORMAT_R32G32_TYPELESS;
        case Format::R32G32_Float: return DXGI_FORMAT_R32G32_FLOAT;
        case Format::R32G32_UInt: return DXGI_FORMAT_R32G32_UINT;
        case Format::R32G32_SInt: return DXGI_FORMAT_R32G32_SINT;
        case Format::R32G8X24_Typeless: return DXGI_FORMAT_R32G8X24_TYPELESS;
        case Format::D32_Float_S8X24_UInt: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case Format::R32_Float_X8X24_Typeless: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case Format::X32_Typeless_G8X24_UInt: return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        case Format::R10G10B10A2_Typeless: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case Format::R10G10B10A2_UNorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case Format::R10G10B10A2_UInt: return DXGI_FORMAT_R10G10B10A2_UINT;
        case Format::R11G11B10_Float: return DXGI_FORMAT_R11G11B10_FLOAT;
        case Format::R8G8B8A8_Typeless: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case Format::R8G8B8A8_UNorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::R8G8B8A8_UNorm_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case Format::R8G8B8A8_UInt: return DXGI_FORMAT_R8G8B8A8_UINT;
        case Format::R8G8B8A8_SNorm: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case Format::R8G8B8A8_SInt: return DXGI_FORMAT_R8G8B8A8_SINT;
        case Format::R16G16_Typeless: return DXGI_FORMAT_R16G16_TYPELESS;
        case Format::R16G16_Float: return DXGI_FORMAT_R16G16_FLOAT;
        case Format::R16G16_UNorm: return DXGI_FORMAT_R16G16_UNORM;
        case Format::R16G16_UInt: return DXGI_FORMAT_R16G16_UINT;
        case Format::R16G16_SNorm: return DXGI_FORMAT_R16G16_SNORM;
        case Format::R16G16_SInt: return DXGI_FORMAT_R16G16_SINT;
        case Format::R32_Typeless: return DXGI_FORMAT_R32_TYPELESS;
        case Format::D32_Float: return DXGI_FORMAT_D32_FLOAT;
        case Format::R32_Float: return DXGI_FORMAT_R32_FLOAT;
        case Format::R32_UInt: return DXGI_FORMAT_R32_UINT;
        case Format::R32_SInt: return DXGI_FORMAT_R32_SINT;
        case Format::R24G8_Typeless: return DXGI_FORMAT_R24G8_TYPELESS;
        case Format::D24_UNorm_S8_UInt: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case Format::R24_UNorm_X8_Typeless: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case Format::X24_Typeless_G8_UInt: return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        case Format::R8G8_Typeless: return DXGI_FORMAT_R8G8_TYPELESS;
        case Format::R8G8_UNorm: return DXGI_FORMAT_R8G8_UNORM;
        case Format::R8G8_UInt: return DXGI_FORMAT_R8G8_UINT;
        case Format::R8G8_SNorm: return DXGI_FORMAT_R8G8_SNORM;
        case Format::R8G8_SInt: return DXGI_FORMAT_R8G8_SINT;
        case Format::R16_Typeless: return DXGI_FORMAT_R16_TYPELESS;
        case Format::R16_Float: return DXGI_FORMAT_R16_FLOAT;
        case Format::D16_UNorm: return DXGI_FORMAT_D16_UNORM;
        case Format::R16_UNorm: return DXGI_FORMAT_R16_UNORM;
        case Format::R16_UInt: return DXGI_FORMAT_R16_UINT;
        case Format::R16_SNorm: return DXGI_FORMAT_R16_SNORM;
        case Format::R16_SInt: return DXGI_FORMAT_R16_SINT;
        case Format::R8_Typeless: return DXGI_FORMAT_R8_TYPELESS;
        case Format::R8_UNorm: return DXGI_FORMAT_R8_UNORM;
        case Format::R8_UInt: return DXGI_FORMAT_R8_UINT;
        case Format::R8_SNorm: return DXGI_FORMAT_R8_SNORM;
        case Format::R8_SInt: return DXGI_FORMAT_R8_SINT;
        case Format::A8_UNorm: return DXGI_FORMAT_A8_UNORM;
        case Format::R1_UNorm: return DXGI_FORMAT_R1_UNORM;
        case Format::R9G9B9E5_SharedExp: return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        case Format::R8G8_B8G8_UNorm: return DXGI_FORMAT_R8G8_B8G8_UNORM;
        case Format::G8R8_G8B8_UNorm: return DXGI_FORMAT_G8R8_G8B8_UNORM;
        case Format::BC1_Typeless: return DXGI_FORMAT_BC1_TYPELESS;
        case Format::BC1_UNorm: return DXGI_FORMAT_BC1_UNORM;
        case Format::BC1_UNorm_SRGB: return DXGI_FORMAT_BC1_UNORM_SRGB;
        case Format::BC2_Typeless: return DXGI_FORMAT_BC2_TYPELESS;
        case Format::BC2_UNorm: return DXGI_FORMAT_BC2_UNORM;
        case Format::BC2_UNorm_SRGB: return DXGI_FORMAT_BC2_UNORM_SRGB;
        case Format::BC3_Typeless: return DXGI_FORMAT_BC3_TYPELESS;
        case Format::BC3_UNorm: return DXGI_FORMAT_BC3_UNORM;
        case Format::BC3_UNorm_SRGB: return DXGI_FORMAT_BC3_UNORM_SRGB;
        case Format::BC4_Typeless: return DXGI_FORMAT_BC4_TYPELESS;
        case Format::BC4_UNorm: return DXGI_FORMAT_BC4_UNORM;
        case Format::BC4_SNorm: return DXGI_FORMAT_BC4_SNORM;
        case Format::BC5_Typeless: return DXGI_FORMAT_BC5_TYPELESS;
        case Format::BC5_UNorm: return DXGI_FORMAT_BC5_UNORM;
        case Format::BC5_SNorm: return DXGI_FORMAT_BC5_SNORM;
        case Format::B5G6R5_UNorm: return DXGI_FORMAT_B5G6R5_UNORM;
        case Format::B5G5R5A1_UNorm: return DXGI_FORMAT_B5G5R5A1_UNORM;
        case Format::B8G8R8A8_UNorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::B8G8R8X8_UNorm: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case Format::R10G10B10_XR_BIAS_A2_UNorm: return DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM;
        case Format::B8G8R8A8_Typeless: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case Format::B8G8R8A8_UNorm_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case Format::B8G8R8X8_Typeless: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
        case Format::B8G8R8X8_UNorm_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
        case Format::BC6H_Typeless: return DXGI_FORMAT_BC6H_TYPELESS;
        case Format::BC6H_UF16: return DXGI_FORMAT_BC6H_UF16;
        case Format::BC6H_SF16: return DXGI_FORMAT_BC6H_SF16;
        case Format::BC7_Typeless: return DXGI_FORMAT_BC7_TYPELESS;
        case Format::BC7_UNorm: return DXGI_FORMAT_BC7_UNORM;
        case Format::BC7_UNorm_SRGB: return DXGI_FORMAT_BC7_UNORM_SRGB;
        case Format::AYUV: return DXGI_FORMAT_AYUV;
        case Format::Y410: return DXGI_FORMAT_Y410;
        case Format::Y416: return DXGI_FORMAT_Y416;
        case Format::NV12: return DXGI_FORMAT_NV12;
        case Format::P010: return DXGI_FORMAT_P010;
        case Format::P016: return DXGI_FORMAT_P016;
        case Format::_420_OPAQUE: return DXGI_FORMAT_420_OPAQUE;
        case Format::YUY2: return DXGI_FORMAT_YUY2;
        case Format::Y210: return DXGI_FORMAT_Y210;
        case Format::Y216: return DXGI_FORMAT_Y216;
        case Format::NV11: return DXGI_FORMAT_NV11;
        case Format::AI44: return DXGI_FORMAT_AI44;
        case Format::IA44: return DXGI_FORMAT_IA44;
        case Format::P8: return DXGI_FORMAT_P8;
        case Format::A8P8: return DXGI_FORMAT_A8P8;
        case Format::B4G4R4A4_UNorm: return DXGI_FORMAT_B4G4R4A4_UNORM;
        case Format::P208: return DXGI_FORMAT_P208;
        case Format::V208: return DXGI_FORMAT_V208;
        case Format::V408: return DXGI_FORMAT_V408;
        case Format::Sampler_FeedBack_Min_Mip_Opaque: return DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE;
        case Format::Sampler_FeedBack_Mip_region_Used_Opaque: return DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE;
        case Format::Force_UInt: return DXGI_FORMAT_FORCE_UINT;
    }
}

Format ToRhiFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
        case DXGI_FORMAT_UNKNOWN: return Format::Unknown;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return Format::R32G32B32A32_Typeless;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return Format::R32G32B32A32_Float;
        case DXGI_FORMAT_R32G32B32A32_UINT: return Format::R32G32B32A32_UInt;
        case DXGI_FORMAT_R32G32B32A32_SINT: return Format::R32G32B32A32_SInt;
        case DXGI_FORMAT_R32G32B32_TYPELESS: return Format::R32G32B32_Typeless;
        case DXGI_FORMAT_R32G32B32_FLOAT: return Format::R32G32B32_Float;
        case DXGI_FORMAT_R32G32B32_UINT: return Format::R32G32B32_UInt;
        case DXGI_FORMAT_R32G32B32_SINT: return Format::R32G32B32_SInt;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return Format::R16G16B16A16_Typeless;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return Format::R16G16B16A16_Float;
        case DXGI_FORMAT_R16G16B16A16_UNORM: return Format::R16G16B16A16_UNorm;
        case DXGI_FORMAT_R16G16B16A16_UINT: return Format::R16G16B16A16_UInt;
        case DXGI_FORMAT_R16G16B16A16_SNORM: return Format::R16G16B16A16_SNorm;
        case DXGI_FORMAT_R16G16B16A16_SINT: return Format::R16G16B16A16_SInt;
        case DXGI_FORMAT_R32G32_TYPELESS: return Format::R32G32_Typeless;
        case DXGI_FORMAT_R32G32_FLOAT: return Format::R32G32_Float;
        case DXGI_FORMAT_R32G32_UINT: return Format::R32G32_UInt;
        case DXGI_FORMAT_R32G32_SINT: return Format::R32G32_SInt;
        case DXGI_FORMAT_R32G8X24_TYPELESS: return Format::R32G8X24_Typeless;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return Format::D32_Float_S8X24_UInt;
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return Format::R32_Float_X8X24_Typeless;
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return Format::X32_Typeless_G8X24_UInt;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return Format::R10G10B10A2_Typeless;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return Format::R10G10B10A2_UNorm;
        case DXGI_FORMAT_R10G10B10A2_UINT: return Format::R10G10B10A2_UInt;
        case DXGI_FORMAT_R11G11B10_FLOAT: return Format::R11G11B10_Float;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return Format::R8G8B8A8_Typeless;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return Format::R8G8B8A8_UNorm;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return Format::R8G8B8A8_UNorm_SRGB;
        case DXGI_FORMAT_R8G8B8A8_UINT: return Format::R8G8B8A8_UInt;
        case DXGI_FORMAT_R8G8B8A8_SNORM: return Format::R8G8B8A8_SNorm;
        case DXGI_FORMAT_R8G8B8A8_SINT: return Format::R8G8B8A8_SInt;
        case DXGI_FORMAT_R16G16_TYPELESS: return Format::R16G16_Typeless;
        case DXGI_FORMAT_R16G16_FLOAT: return Format::R16G16_Float;
        case DXGI_FORMAT_R16G16_UNORM: return Format::R16G16_UNorm;
        case DXGI_FORMAT_R16G16_UINT: return Format::R16G16_UInt;
        case DXGI_FORMAT_R16G16_SNORM: return Format::R16G16_SNorm;
        case DXGI_FORMAT_R16G16_SINT: return Format::R16G16_SInt;
        case DXGI_FORMAT_R32_TYPELESS: return Format::R32_Typeless;
        case DXGI_FORMAT_D32_FLOAT: return Format::D32_Float;
        case DXGI_FORMAT_R32_FLOAT: return Format::R32_Float;
        case DXGI_FORMAT_R32_UINT: return Format::R32_UInt;
        case DXGI_FORMAT_R32_SINT: return Format::R32_SInt;
        case DXGI_FORMAT_R24G8_TYPELESS: return Format::R24G8_Typeless;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return Format::D24_UNorm_S8_UInt;
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return Format::R24_UNorm_X8_Typeless;
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return Format::X24_Typeless_G8_UInt;
        case DXGI_FORMAT_R8G8_TYPELESS: return Format::R8G8_Typeless;
        case DXGI_FORMAT_R8G8_UNORM: return Format::R8G8_UNorm;
        case DXGI_FORMAT_R8G8_UINT: return Format::R8G8_UInt;
        case DXGI_FORMAT_R8G8_SNORM: return Format::R8G8_SNorm;
        case DXGI_FORMAT_R8G8_SINT: return Format::R8G8_SInt;
        case DXGI_FORMAT_R16_TYPELESS: return Format::R16_Typeless;
        case DXGI_FORMAT_R16_FLOAT: return Format::R16_Float;
        case DXGI_FORMAT_D16_UNORM: return Format::D16_UNorm;
        case DXGI_FORMAT_R16_UNORM: return Format::R16_UNorm;
        case DXGI_FORMAT_R16_UINT: return Format::R16_UInt;
        case DXGI_FORMAT_R16_SNORM: return Format::R16_SNorm;
        case DXGI_FORMAT_R16_SINT: return Format::R16_SInt;
        case DXGI_FORMAT_R8_TYPELESS: return Format::R8_Typeless;
        case DXGI_FORMAT_R8_UNORM: return Format::R8_UNorm;
        case DXGI_FORMAT_R8_UINT: return Format::R8_UInt;
        case DXGI_FORMAT_R8_SNORM: return Format::R8_SNorm;
        case DXGI_FORMAT_R8_SINT: return Format::R8_SInt;
        case DXGI_FORMAT_A8_UNORM: return Format::A8_UNorm;
        case DXGI_FORMAT_R1_UNORM: return Format::R1_UNorm;
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return Format::R9G9B9E5_SharedExp;
        case DXGI_FORMAT_R8G8_B8G8_UNORM: return Format::R8G8_B8G8_UNorm;
        case DXGI_FORMAT_G8R8_G8B8_UNORM: return Format::G8R8_G8B8_UNorm;
        case DXGI_FORMAT_BC1_TYPELESS: return Format::BC1_Typeless;
        case DXGI_FORMAT_BC1_UNORM: return Format::BC1_UNorm;
        case DXGI_FORMAT_BC1_UNORM_SRGB: return Format::BC1_UNorm_SRGB;
        case DXGI_FORMAT_BC2_TYPELESS: return Format::BC2_Typeless;
        case DXGI_FORMAT_BC2_UNORM: return Format::BC2_UNorm;
        case DXGI_FORMAT_BC2_UNORM_SRGB: return Format::BC2_UNorm_SRGB;
        case DXGI_FORMAT_BC3_TYPELESS: return Format::BC3_Typeless;
        case DXGI_FORMAT_BC3_UNORM: return Format::BC3_UNorm;
        case DXGI_FORMAT_BC3_UNORM_SRGB: return Format::BC3_UNorm_SRGB;
        case DXGI_FORMAT_BC4_TYPELESS: return Format::BC4_Typeless;
        case DXGI_FORMAT_BC4_UNORM: return Format::BC4_UNorm;
        case DXGI_FORMAT_BC4_SNORM: return Format::BC4_SNorm;
        case DXGI_FORMAT_BC5_TYPELESS: return Format::BC5_Typeless;
        case DXGI_FORMAT_BC5_UNORM: return Format::BC5_UNorm;
        case DXGI_FORMAT_BC5_SNORM: return Format::BC5_SNorm;
        case DXGI_FORMAT_B5G6R5_UNORM: return Format::B5G6R5_UNorm;
        case DXGI_FORMAT_B5G5R5A1_UNORM: return Format::B5G5R5A1_UNorm;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return Format::B8G8R8A8_UNorm;
        case DXGI_FORMAT_B8G8R8X8_UNORM: return Format::B8G8R8X8_UNorm;
        case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM: return Format::R10G10B10_XR_BIAS_A2_UNorm;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return Format::B8G8R8A8_Typeless;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return Format::B8G8R8A8_UNorm_SRGB;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return Format::B8G8R8X8_Typeless;
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return Format::B8G8R8X8_UNorm_SRGB;
        case DXGI_FORMAT_BC6H_TYPELESS: return Format::BC6H_Typeless;
        case DXGI_FORMAT_BC6H_UF16: return Format::BC6H_UF16;
        case DXGI_FORMAT_BC6H_SF16: return Format::BC6H_SF16;
        case DXGI_FORMAT_BC7_TYPELESS: return Format::BC7_Typeless;
        case DXGI_FORMAT_BC7_UNORM: return Format::BC7_UNorm;
        case DXGI_FORMAT_BC7_UNORM_SRGB: return Format::BC7_UNorm_SRGB;
        case DXGI_FORMAT_AYUV: return Format::AYUV;
        case DXGI_FORMAT_Y410: return Format::Y410;
        case DXGI_FORMAT_Y416: return Format::Y416;
        case DXGI_FORMAT_NV12: return Format::NV12;
        case DXGI_FORMAT_P010: return Format::P010;
        case DXGI_FORMAT_P016: return Format::P016;
        case DXGI_FORMAT_420_OPAQUE: return Format::_420_OPAQUE;
        case DXGI_FORMAT_YUY2: return Format::YUY2;
        case DXGI_FORMAT_Y210: return Format::Y210;
        case DXGI_FORMAT_Y216: return Format::Y216;
        case DXGI_FORMAT_NV11: return Format::NV11;
        case DXGI_FORMAT_AI44: return Format::AI44;
        case DXGI_FORMAT_IA44: return Format::IA44;
        case DXGI_FORMAT_P8: return Format::P8;
        case DXGI_FORMAT_A8P8: return Format::A8P8;
        case DXGI_FORMAT_B4G4R4A4_UNORM: return Format::B4G4R4A4_UNorm;
        case DXGI_FORMAT_P208: return Format::P208;
        case DXGI_FORMAT_V208: return Format::V208;
        case DXGI_FORMAT_V408: return Format::V408;
        case DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE: return Format::Sampler_FeedBack_Min_Mip_Opaque;
        case DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE: return Format::Sampler_FeedBack_Mip_region_Used_Opaque;
        case DXGI_FORMAT_FORCE_UINT: return Format::Force_UInt;
    }
}

}  // namespace radray::rhi::d3d12
