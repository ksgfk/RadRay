#include "d3d12_helper.h"

namespace radray::render::d3d12 {

Win32Event::Win32Event(Win32Event&& other) noexcept
    : _event(other._event) {
    other._event = nullptr;
}

Win32Event& Win32Event::operator=(Win32Event&& other) noexcept {
    if (this != &other) {
        _event = other._event;
        other._event = nullptr;
    }
    return *this;
}

Win32Event::~Win32Event() noexcept {
    Destroy();
}

void Win32Event::Destroy() noexcept {
    if (_event) {
        CloseHandle(_event);
        _event = nullptr;
    }
}

std::optional<Win32Event> MakeWin32Event() noexcept {
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        DWORD err = GetLastError();
        RADRAY_ERR_LOG("cannot create WIN32 event, code:{}", err);
        return std::nullopt;
    }
    Win32Event result{};
    result._event = event;
    return std::make_optional(std::move(result));
}

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
    if (str.length() == 0) {
        if (alloc) {
            alloc->SetName(nullptr);
        }
        obj->SetName(L"");
    } else {
        std::optional<wstring> wco = ToWideChar(str);
        if (wco.has_value()) {
            const wchar_t* debugName = wco.value().c_str();
            if (alloc) {
                alloc->SetName(debugName);
            }
            obj->SetName(debugName);
        }
    }
}

DXGI_FORMAT FormatToTypeless(DXGI_FORMAT fmt) noexcept {
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
        case DXGI_FORMAT_B4G4R4A4_UNORM:
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
        case DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE:
        case DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE:
        case DXGI_FORMAT_FORCE_UINT:
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT MapShaderResourceType(TextureFormat v) noexcept {
    DXGI_FORMAT fmt = MapType(v);
    switch (v) {
        case TextureFormat::D16_UNORM: fmt = DXGI_FORMAT_R16_UNORM; break;
        case TextureFormat::D32_FLOAT: fmt = DXGI_FORMAT_R32_FLOAT; break;
        case TextureFormat::D24_UNORM_S8_UINT: fmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        case TextureFormat::D32_FLOAT_S8_UINT: fmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        default: break;
    }
    return fmt;
}

UINT SubresourceIndex(UINT MipSlice, UINT ArraySlice, UINT PlaneSlice, UINT MipLevels, UINT ArraySize) noexcept {
    return ((MipSlice) + ((ArraySlice) * (MipLevels)) + ((PlaneSlice) * (MipLevels) * (ArraySize)));
}

D3D12_COMMAND_LIST_TYPE MapType(QueueType v) noexcept {
    switch (v) {
        case QueueType::Direct: return D3D12_COMMAND_LIST_TYPE_DIRECT;
        case QueueType::Compute: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
        case QueueType::Copy: return D3D12_COMMAND_LIST_TYPE_COPY;
        default: return D3D12_COMMAND_LIST_TYPE_NONE;
    }
}

DXGI_FORMAT MapType(TextureFormat v) noexcept {
    switch (v) {
        case TextureFormat::UNKNOWN: return DXGI_FORMAT_UNKNOWN;
        case TextureFormat::R8_SINT: return DXGI_FORMAT_R8_SINT;
        case TextureFormat::R8_UINT: return DXGI_FORMAT_R8_UINT;
        case TextureFormat::R8_SNORM: return DXGI_FORMAT_R8_SNORM;
        case TextureFormat::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
        case TextureFormat::R16_SINT: return DXGI_FORMAT_R16_SINT;
        case TextureFormat::R16_UINT: return DXGI_FORMAT_R16_UINT;
        case TextureFormat::R16_SNORM: return DXGI_FORMAT_R16_SNORM;
        case TextureFormat::R16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case TextureFormat::R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
        case TextureFormat::RG8_SINT: return DXGI_FORMAT_R8G8_SINT;
        case TextureFormat::RG8_UINT: return DXGI_FORMAT_R8G8_UINT;
        case TextureFormat::RG8_SNORM: return DXGI_FORMAT_R8G8_SNORM;
        case TextureFormat::RG8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
        case TextureFormat::R32_SINT: return DXGI_FORMAT_R32_SINT;
        case TextureFormat::R32_UINT: return DXGI_FORMAT_R32_UINT;
        case TextureFormat::R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case TextureFormat::RG16_SINT: return DXGI_FORMAT_R16G16_SINT;
        case TextureFormat::RG16_UINT: return DXGI_FORMAT_R16G16_UINT;
        case TextureFormat::RG16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
        case TextureFormat::RG16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
        case TextureFormat::RG16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
        case TextureFormat::RGBA8_SINT: return DXGI_FORMAT_R8G8B8A8_SINT;
        case TextureFormat::RGBA8_UINT: return DXGI_FORMAT_R8G8B8A8_UINT;
        case TextureFormat::RGBA8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case TextureFormat::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::RGBA8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case TextureFormat::BGRA8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case TextureFormat::BGRA8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case TextureFormat::RGB10A2_UINT: return DXGI_FORMAT_R10G10B10A2_UINT;
        case TextureFormat::RGB10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case TextureFormat::RG11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
        case TextureFormat::RG32_SINT: return DXGI_FORMAT_R32G32_SINT;
        case TextureFormat::RG32_UINT: return DXGI_FORMAT_R32G32_UINT;
        case TextureFormat::RG32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
        case TextureFormat::RGBA16_SINT: return DXGI_FORMAT_R16G16B16A16_SINT;
        case TextureFormat::RGBA16_UINT: return DXGI_FORMAT_R16G16B16A16_UINT;
        case TextureFormat::RGBA16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case TextureFormat::RGBA16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case TextureFormat::RGBA16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case TextureFormat::RGBA32_SINT: return DXGI_FORMAT_R32G32B32A32_SINT;
        case TextureFormat::RGBA32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
        case TextureFormat::RGBA32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case TextureFormat::S8: return DXGI_FORMAT_R8_UINT;
        case TextureFormat::D16_UNORM: return DXGI_FORMAT_D16_UNORM;
        case TextureFormat::D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
        case TextureFormat::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case TextureFormat::D32_FLOAT_S8_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

D3D12_HEAP_TYPE MapType(MemoryType v) noexcept {
    switch (v) {
        case MemoryType::Device: return D3D12_HEAP_TYPE_DEFAULT;
        case MemoryType::Upload: return D3D12_HEAP_TYPE_UPLOAD;
        case MemoryType::ReadBack: return D3D12_HEAP_TYPE_READBACK;
        default: return D3D12_HEAP_TYPE_DEFAULT;
    }
}

D3D12_RESOURCE_DIMENSION MapType(TextureDimension v) noexcept {
    switch (v) {
        case TextureDimension::Dim1D: return D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        case TextureDimension::Dim2D: return D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        case TextureDimension::Dim3D: return D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        default: return D3D12_RESOURCE_DIMENSION_UNKNOWN;
    }
}

D3D12_SHADER_VISIBILITY MapShaderStages(ShaderStages v) noexcept {
    if (v == ShaderStage::Compute) {
        return D3D12_SHADER_VISIBILITY_ALL;
    }
    if (v == ShaderStage::UNKNOWN) {
        return D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_SHADER_VISIBILITY res = D3D12_SHADER_VISIBILITY_ALL;
    uint32_t stageCount = 0;
    if (v.HasFlag(ShaderStage::Vertex)) {
        res = D3D12_SHADER_VISIBILITY_VERTEX;
        ++stageCount;
    }
    if (v.HasFlag(ShaderStage::Pixel)) {
        res = D3D12_SHADER_VISIBILITY_PIXEL;
        ++stageCount;
    }
    return stageCount > 1 ? D3D12_SHADER_VISIBILITY_ALL : res;
}

std::pair<D3D12_PRIMITIVE_TOPOLOGY_TYPE, D3D12_PRIMITIVE_TOPOLOGY> MapType(PrimitiveTopology v) noexcept {
    switch (v) {
        case PrimitiveTopology::PointList: return {D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, D3D_PRIMITIVE_TOPOLOGY_POINTLIST};
        case PrimitiveTopology::LineList: return {D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, D3D_PRIMITIVE_TOPOLOGY_LINELIST};
        case PrimitiveTopology::LineStrip: return {D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, D3D_PRIMITIVE_TOPOLOGY_LINESTRIP};
        case PrimitiveTopology::TriangleList: return {D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST};
        case PrimitiveTopology::TriangleStrip: return {D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP};
        default: return {D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED, D3D_PRIMITIVE_TOPOLOGY_UNDEFINED};
    }
}

D3D12_INPUT_CLASSIFICATION MapType(VertexStepMode v) noexcept {
    switch (v) {
        case VertexStepMode::Vertex: return D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        case VertexStepMode::Instance: return D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
        default: return D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    }
}

DXGI_FORMAT MapType(VertexFormat v) noexcept {
    switch (v) {
        case VertexFormat::UNKNOWN: return DXGI_FORMAT_UNKNOWN;
        case VertexFormat::UINT8X2: return DXGI_FORMAT_R8G8_UINT;
        case VertexFormat::UINT8X4: return DXGI_FORMAT_R8G8B8A8_UINT;
        case VertexFormat::SINT8X2: return DXGI_FORMAT_R8G8_SINT;
        case VertexFormat::SINT8X4: return DXGI_FORMAT_R8G8B8A8_SINT;
        case VertexFormat::UNORM8X2: return DXGI_FORMAT_R8G8_UNORM;
        case VertexFormat::UNORM8X4: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VertexFormat::SNORM8X2: return DXGI_FORMAT_R8G8_SNORM;
        case VertexFormat::SNORM8X4: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case VertexFormat::UINT16X2: return DXGI_FORMAT_R16G16_UINT;
        case VertexFormat::UINT16X4: return DXGI_FORMAT_R16G16B16A16_UINT;
        case VertexFormat::SINT16X2: return DXGI_FORMAT_R16G16_SINT;
        case VertexFormat::SINT16X4: return DXGI_FORMAT_R16G16B16A16_SINT;
        case VertexFormat::UNORM16X2: return DXGI_FORMAT_R16G16_UNORM;
        case VertexFormat::UNORM16X4: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case VertexFormat::SNORM16X2: return DXGI_FORMAT_R16G16_SNORM;
        case VertexFormat::SNORM16X4: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case VertexFormat::FLOAT16X2: return DXGI_FORMAT_R16G16_FLOAT;
        case VertexFormat::FLOAT16X4: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case VertexFormat::UINT32: return DXGI_FORMAT_R32_UINT;
        case VertexFormat::UINT32X2: return DXGI_FORMAT_R32G32_UINT;
        case VertexFormat::UINT32X3: return DXGI_FORMAT_R32G32B32_UINT;
        case VertexFormat::UINT32X4: return DXGI_FORMAT_R32G32B32A32_UINT;
        case VertexFormat::SINT32: return DXGI_FORMAT_R32_SINT;
        case VertexFormat::SINT32X2: return DXGI_FORMAT_R32G32_SINT;
        case VertexFormat::SINT32X3: return DXGI_FORMAT_R32G32B32_SINT;
        case VertexFormat::SINT32X4: return DXGI_FORMAT_R32G32B32A32_SINT;
        case VertexFormat::FLOAT32: return DXGI_FORMAT_R32_FLOAT;
        case VertexFormat::FLOAT32X2: return DXGI_FORMAT_R32G32_FLOAT;
        case VertexFormat::FLOAT32X3: return DXGI_FORMAT_R32G32B32_FLOAT;
        case VertexFormat::FLOAT32X4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

std::optional<D3D12_FILL_MODE> MapType(PolygonMode v) noexcept {
    switch (v) {
        case PolygonMode::Fill: return D3D12_FILL_MODE_SOLID;
        case PolygonMode::Line: return D3D12_FILL_MODE_WIREFRAME;
        case PolygonMode::Point: return std::nullopt;
        default: return std::nullopt;
    }
}

D3D12_CULL_MODE MapType(CullMode v) noexcept {
    switch (v) {
        case CullMode::Front: return D3D12_CULL_MODE_FRONT;
        case CullMode::Back: return D3D12_CULL_MODE_BACK;
        case CullMode::None: return D3D12_CULL_MODE_NONE;
        default: return D3D12_CULL_MODE_NONE;
    }
}

D3D12_BLEND_OP MapType(BlendOperation v) noexcept {
    switch (v) {
        case BlendOperation::Add: return D3D12_BLEND_OP_ADD;
        case BlendOperation::Subtract: return D3D12_BLEND_OP_SUBTRACT;
        case BlendOperation::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
        case BlendOperation::Min: return D3D12_BLEND_OP_MIN;
        case BlendOperation::Max: return D3D12_BLEND_OP_MAX;
        default: return D3D12_BLEND_OP_ADD;
    }
}

D3D12_BLEND MapBlendColor(BlendFactor v) noexcept {
    switch (v) {
        case BlendFactor::Zero: return D3D12_BLEND_ZERO;
        case BlendFactor::One: return D3D12_BLEND_ONE;
        case BlendFactor::Src: return D3D12_BLEND_SRC_COLOR;
        case BlendFactor::OneMinusSrc: return D3D12_BLEND_INV_SRC_COLOR;
        case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::Dst: return D3D12_BLEND_DEST_COLOR;
        case BlendFactor::OneMinusDst: return D3D12_BLEND_INV_DEST_COLOR;
        case BlendFactor::DstAlpha: return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
        case BlendFactor::SrcAlphaSaturated: return D3D12_BLEND_SRC_ALPHA_SAT;
        case BlendFactor::Constant: return D3D12_BLEND_BLEND_FACTOR;
        case BlendFactor::OneMinusConstant: return D3D12_BLEND_INV_BLEND_FACTOR;
        case BlendFactor::Src1: return D3D12_BLEND_SRC1_COLOR;
        case BlendFactor::OneMinusSrc1: return D3D12_BLEND_INV_SRC1_COLOR;
        case BlendFactor::Src1Alpha: return D3D12_BLEND_SRC1_ALPHA;
        case BlendFactor::OneMinusSrc1Alpha: return D3D12_BLEND_INV_SRC1_ALPHA;
        default: return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND MapBlendAlpha(BlendFactor v) noexcept {
    switch (v) {
        case BlendFactor::Zero: return D3D12_BLEND_ZERO;
        case BlendFactor::One: return D3D12_BLEND_ONE;
        case BlendFactor::Src: return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSrc: return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::Dst: return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDst: return D3D12_BLEND_INV_DEST_ALPHA;
        case BlendFactor::DstAlpha: return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
        case BlendFactor::SrcAlphaSaturated: return D3D12_BLEND_SRC_ALPHA_SAT;
        case BlendFactor::Constant: return D3D12_BLEND_BLEND_FACTOR;
        case BlendFactor::OneMinusConstant: return D3D12_BLEND_INV_BLEND_FACTOR;
        case BlendFactor::Src1: return D3D12_BLEND_SRC1_ALPHA;
        case BlendFactor::OneMinusSrc1: return D3D12_BLEND_INV_SRC1_ALPHA;
        case BlendFactor::Src1Alpha: return D3D12_BLEND_SRC1_ALPHA;
        case BlendFactor::OneMinusSrc1Alpha: return D3D12_BLEND_INV_SRC1_ALPHA;
        default: return D3D12_BLEND_ONE;
    }
}

std::optional<D3D12_COLOR_WRITE_ENABLE> MapColorWrites(ColorWrites v) noexcept {
    if (v == ColorWrite::Red) return D3D12_COLOR_WRITE_ENABLE_RED;
    if (v == ColorWrite::Green) return D3D12_COLOR_WRITE_ENABLE_GREEN;
    if (v == ColorWrite::Blue) return D3D12_COLOR_WRITE_ENABLE_BLUE;
    if (v == ColorWrite::Alpha) return D3D12_COLOR_WRITE_ENABLE_ALPHA;
    if (v == ColorWrite::All) return D3D12_COLOR_WRITE_ENABLE_ALL;
    return std::nullopt;
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
        default: return D3D12_COMPARISON_FUNC_NONE;
    }
}

D3D12_STENCIL_OP MapType(StencilOperation v) noexcept {
    switch (v) {
        case StencilOperation::Keep: return D3D12_STENCIL_OP_KEEP;
        case StencilOperation::Zero: return D3D12_STENCIL_OP_ZERO;
        case StencilOperation::Replace: return D3D12_STENCIL_OP_REPLACE;
        case StencilOperation::Invert: return D3D12_STENCIL_OP_INVERT;
        case StencilOperation::IncrementClamp: return D3D12_STENCIL_OP_INCR_SAT;
        case StencilOperation::DecrementClamp: return D3D12_STENCIL_OP_DECR_SAT;
        case StencilOperation::IncrementWrap: return D3D12_STENCIL_OP_INCR;
        case StencilOperation::DecrementWrap: return D3D12_STENCIL_OP_DECR;
        default: return D3D12_STENCIL_OP_KEEP;
    }
}

D3D12_INDEX_BUFFER_STRIP_CUT_VALUE MapType(IndexFormat v) noexcept {
    switch (v) {
        case IndexFormat::UINT16: return D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
        case IndexFormat::UINT32: return D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
        default: return D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    }
}

D3D12_RESOURCE_STATES MapType(BufferUses v) noexcept {
    D3D12_RESOURCE_STATES result = D3D12_RESOURCE_STATE_COMMON;
    if (v.HasFlag(BufferUse::CopySource)) {
        result |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    if (v.HasFlag(BufferUse::CopyDestination)) {
        result |= D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if (v.HasFlag(BufferUse::Index)) {
        result |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    }
    if (v.HasFlag(BufferUse::Vertex) || v.HasFlag(BufferUse::CBuffer)) {
        result |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    }
    if (v.HasFlag(BufferUse::Resource)) {
        result |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    if (v.HasFlag(BufferUse::UnorderedAccess)) {
        result |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    if (v.HasFlag(BufferUse::Indirect)) {
        result |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    }
    return result;
}

D3D12_RESOURCE_STATES MapType(TextureUses v) noexcept {
    D3D12_RESOURCE_STATES result = D3D12_RESOURCE_STATE_COMMON;
    if (v.HasFlag(TextureUse::Present)) {
        result |= D3D12_RESOURCE_STATE_PRESENT;
    }
    if (v.HasFlag(TextureUse::CopySource)) {
        result |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    if (v.HasFlag(TextureUse::CopyDestination)) {
        result |= D3D12_RESOURCE_STATE_COPY_DEST;
    }
    if (v.HasFlag(TextureUse::Resource)) {
        result |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    if (v.HasFlag(TextureUse::RenderTarget)) {
        result |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (v.HasFlag(TextureUse::DepthStencilRead)) {
        result |= D3D12_RESOURCE_STATE_DEPTH_READ;
    }
    if (v.HasFlag(TextureUse::DepthStencilWrite)) {
        result |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    if (v.HasFlag(TextureUse::UnorderedAccess)) {
        result |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    return result;
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
        default: return "UNKNOWN";
    }
}

std::string_view format_as(D3D12_RESOURCE_BINDING_TIER v) noexcept {
    switch (v) {
        case D3D12_RESOURCE_BINDING_TIER_1: return "1";
        case D3D12_RESOURCE_BINDING_TIER_2: return "2";
        case D3D12_RESOURCE_BINDING_TIER_3: return "3";
        default: return "UNKNOWN";
    }
}

std::string_view format_as(D3D12_DESCRIPTOR_HEAP_TYPE v) noexcept {
    switch (v) {
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV: return "CBV_SRV_UAV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER: return "SAMPLER";
        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV: return "RTV";
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV: return "DSV";
        default: return "UNKNOWN";
    }
}
