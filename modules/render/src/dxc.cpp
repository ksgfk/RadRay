#include <radray/render/dxc.h>

namespace radray::render {

std::optional<DxcReflectionRadrayExt> DeserializeDxcReflectionRadrayExt(std::span<const byte> data) noexcept {
    const byte* ptr = data.data();
    const byte* end = ptr + data.size();
    auto readU32 = [&]() -> uint32_t {
        if (ptr + sizeof(uint32_t) > end) return 0;
        uint32_t v;
        std::memcpy(&v, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        return v;
    };
    auto readU8 = [&]() -> uint8_t {
        if (ptr + sizeof(uint8_t) > end) return 0;
        uint8_t v;
        std::memcpy(&v, ptr, sizeof(uint8_t));
        ptr += sizeof(uint8_t);
        return v;
    };
    uint32_t magic = readU32();
    constexpr uint32_t Magic = 0x52524558; // RREX
    if (magic != Magic) {
        return std::nullopt;
    }
    DxcReflectionRadrayExt result;
    result.TargetType = readU8();
    uint32_t count = readU32();
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t nameLen = readU32();
        string name;
        if (ptr + nameLen <= end) {
            name.assign(reinterpret_cast<const char*>(ptr), nameLen);
            ptr += nameLen;
        }
        uint32_t bindPoint = readU32();
        uint32_t space = readU32();
        uint8_t isViewInHlsl = readU8();
        DxcReflectionRadrayExtCBuffer& cbExt = result.CBuffers.emplace_back();
        cbExt.Name = std::move(name);
        cbExt.BindPoint = bindPoint;
        cbExt.Space = space;
        cbExt.IsViewInHlsl = isViewInHlsl != 0;
    }
    return result;
}

bool HlslShaderTypeDesc::IsPrimitive() const noexcept {
    return Class == HlslShaderVariableClass::SCALAR ||
           Class == HlslShaderVariableClass::VECTOR ||
           Class == HlslShaderVariableClass::MATRIX_ROWS ||
           Class == HlslShaderVariableClass::MATRIX_COLUMNS;
}

size_t HlslShaderTypeDesc::GetSizeInBytes() const noexcept {
    switch (Type) {
        case HlslShaderVariableType::INT16:
        case HlslShaderVariableType::UINT16:
        case HlslShaderVariableType::FLOAT16:
            return 2 * Columns * Rows;
        case HlslShaderVariableType::UINT8:
            return 1 * Columns * Rows;
        case HlslShaderVariableType::DOUBLE:
        case HlslShaderVariableType::INT64:
        case HlslShaderVariableType::UINT64:
            return 8 * Columns * Rows;
        case HlslShaderVariableType::BOOL:
        case HlslShaderVariableType::INT:
        case HlslShaderVariableType::FLOAT:
        case HlslShaderVariableType::UINT:
            return 4 * Columns * Rows;
        default:
            return 0;
    }
}

ResourceBindType HlslInputBindDesc::MapResourceBindType() const noexcept {
    switch (Type) {
        case HlslShaderInputType::CBUFFER: return ResourceBindType::CBuffer;
        case HlslShaderInputType::TBUFFER:
        case HlslShaderInputType::STRUCTURED:
        case HlslShaderInputType::BYTEADDRESS: return ResourceBindType::Buffer;
        case HlslShaderInputType::TEXTURE: return IsBufferDimension(Dimension) ? ResourceBindType::Buffer : ResourceBindType::Texture;
        case HlslShaderInputType::SAMPLER: return ResourceBindType::Sampler;
        case HlslShaderInputType::UAV_RWTYPED: return IsBufferDimension(Dimension) ? ResourceBindType::RWBuffer : ResourceBindType::RWTexture;
        case HlslShaderInputType::UAV_RWSTRUCTURED:
        case HlslShaderInputType::UAV_RWSTRUCTURED_WITH_COUNTER:
        case HlslShaderInputType::UAV_APPEND_STRUCTURED:
        case HlslShaderInputType::UAV_CONSUME_STRUCTURED:
        case HlslShaderInputType::UAV_RWBYTEADDRESS: return ResourceBindType::RWBuffer;
        case HlslShaderInputType::RTACCELERATIONSTRUCTURE: return ResourceBindType::Buffer;
        case HlslShaderInputType::UAV_FEEDBACKTEXTURE: return ResourceBindType::RWTexture;
        case HlslShaderInputType::UNKNOWN: return ResourceBindType::UNKNOWN;
    }
    Unreachable();
}

static std::optional<std::reference_wrapper<const HlslShaderBufferDesc>> _FindCBufferByName(std::span<const HlslShaderBufferDesc> data, std::string_view name) noexcept {
    auto it = std::find_if(data.begin(), data.end(), [&](const HlslShaderBufferDesc& cb) {
        return cb.Name == name;
    });
    return it == data.end() ? std::nullopt : std::make_optional(std::cref(*it));
}

std::optional<std::reference_wrapper<const HlslShaderBufferDesc>> HlslShaderDesc::FindCBufferByName(std::string_view name) const noexcept {
    return _FindCBufferByName(ConstantBuffers, name);
}

bool operator==(const radray::render::HlslShaderTypeDesc& lhs, const radray::render::HlslShaderTypeDesc& rhs) noexcept {
    if (lhs.Name != rhs.Name ||
        lhs.Class != rhs.Class ||
        lhs.Type != rhs.Type ||
        lhs.Rows != rhs.Rows ||
        lhs.Columns != rhs.Columns ||
        lhs.Elements != rhs.Elements ||
        lhs.Offset != rhs.Offset ||
        lhs.Members.size() != rhs.Members.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.Members.size(); i++) {
        const auto& lm = lhs.Members[i];
        const auto& rm = rhs.Members[i];
        if ((*lm.Type) != (*rm.Type)) {
            return false;
        }
    }
    return true;
}

bool operator!=(const radray::render::HlslShaderTypeDesc& lhs, const radray::render::HlslShaderTypeDesc& rhs) noexcept {
    return !(operator==(lhs, rhs));
}

bool operator==(const radray::render::HlslShaderVariableDesc& lhs, const radray::render::HlslShaderVariableDesc& rhs) noexcept {
    if (lhs.Name != rhs.Name ||
        lhs.StartOffset != rhs.StartOffset ||
        lhs.Size != rhs.Size ||
        lhs.StartTexture != rhs.StartTexture ||
        lhs.TextureSize != rhs.TextureSize ||
        lhs.StartSampler != rhs.StartSampler ||
        lhs.SamplerSize != rhs.SamplerSize) {
        return false;
    }
    return (*lhs.Type) == (*rhs.Type);
}

bool operator!=(const radray::render::HlslShaderVariableDesc& lhs, const radray::render::HlslShaderVariableDesc& rhs) noexcept {
    return !(operator==(lhs, rhs));
}

bool operator==(const radray::render::HlslShaderBufferDesc& lhs, const radray::render::HlslShaderBufferDesc& rhs) noexcept {
    if (lhs.Name != rhs.Name || lhs.Size != rhs.Size || lhs.Type != rhs.Type || lhs.Variables.size() != rhs.Variables.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.Variables.size(); i++) {
        const auto& lv = lhs.Variables[i];
        const auto& rv = rhs.Variables[i];
        if ((*lv) != (*rv)) {
            return false;
        }
    }
    return true;
}

bool operator!=(const radray::render::HlslShaderBufferDesc& lhs, const radray::render::HlslShaderBufferDesc& rhs) noexcept {
    return !(operator==(lhs, rhs));
}

bool operator==(const HlslInputBindDesc& lhs, const HlslInputBindDesc& rhs) noexcept {
    return lhs.Name == rhs.Name &&
           lhs.Type == rhs.Type &&
           lhs.BindPoint == rhs.BindPoint &&
           lhs.BindCount == rhs.BindCount &&
           lhs.ReturnType == rhs.ReturnType &&
           lhs.Dimension == rhs.Dimension &&
           lhs.NumSamples == rhs.NumSamples &&
           lhs.Space == rhs.Space;
}

bool operator!=(const HlslInputBindDesc& lhs, const HlslInputBindDesc& rhs) noexcept {
    return !(operator==(lhs, rhs));
}

bool IsBufferDimension(HlslSRVDimension dim) noexcept {
    switch (dim) {
        case HlslSRVDimension::BUFFER:
        case HlslSRVDimension::BUFFEREX: return true;
        default: return false;
    }
}

std::optional<std::reference_wrapper<const HlslShaderBufferDesc>> MergedHlslShaderDesc::FindCBufferByName(std::string_view name) const noexcept {
    return _FindCBufferByName(ConstantBuffers, name);
}

MergedHlslShaderDesc MergeHlslShaderDesc(std::span<const HlslShaderDesc*> descs) noexcept {
    MergedHlslShaderDesc result{};
    for (auto descPtr : descs) {
        result.Stages |= descPtr->Stage;
        for (const auto& cb : descPtr->ConstantBuffers) {
            auto it = std::find_if(result.ConstantBuffers.begin(), result.ConstantBuffers.end(), [&](const HlslShaderBufferDesc& existing) {
                return existing == cb;
            });
            if (it == result.ConstantBuffers.end()) {
                result.ConstantBuffers.emplace_back(cb);
            }
        }
        for (const auto& br : descPtr->BoundResources) {
            auto it = std::find_if(result.BoundResources.begin(), result.BoundResources.end(), [&](const HlslInputBindWithStageDesc& existing) {
                return existing == br;
            });
            if (it == result.BoundResources.end()) {
                HlslInputBindWithStageDesc newBr{br};
                newBr.Stages = descPtr->Stage;
                result.BoundResources.emplace_back(newBr);
            } else {
                it->Stages |= descPtr->Stage;
            }
        }
    }

    size_t varCount = 0;
    for (const auto& cbuffer : result.ConstantBuffers) {
        varCount += cbuffer.Variables.size();
    }
    result.Variables.resize(varCount);
    size_t varBase = 0;
    for (auto& cbuffer : result.ConstantBuffers) {
        for (size_t i = 0; i < cbuffer.Variables.size(); i++) {
            auto dstIndex = varBase + i;
            auto cbVar = cbuffer.Variables[i];
            result.Variables[dstIndex] = *cbVar;
            cbuffer.Variables[i] = &result.Variables[dstIndex];
        }
        varBase += cbuffer.Variables.size();
    }

    unordered_map<const HlslShaderTypeDesc*, size_t> typeIndexCache;
    vector<const HlslShaderTypeDesc*> orderedTypes;
    std::function<void(const HlslShaderTypeDesc*)> visitType;
    visitType = [&](const HlslShaderTypeDesc* type) {
        auto [it, inserted] = typeIndexCache.emplace(type, orderedTypes.size());
        if (!inserted) {
            return;
        }
        orderedTypes.push_back(type);
        for (const auto& member : type->Members) {
            visitType(member.Type);
        }
    };
    for (auto& cbuffer : result.ConstantBuffers) {
        for (const auto* var : cbuffer.Variables) {
            visitType(var->Type);
        }
    }
    result.Types.resize(orderedTypes.size());
    for (size_t i = 0; i < orderedTypes.size(); i++) {
        result.Types[i] = *orderedTypes[i];
    }
    for (size_t i = 0; i < result.Types.size(); i++) {
        auto& typeDesc = result.Types[i];
        for (auto& member : typeDesc.Members) {
            const auto* srcMemberType = member.Type;
            auto it = typeIndexCache.find(srcMemberType);
            RADRAY_ASSERT(it != typeIndexCache.end());
            member.Type = &result.Types[it->second];
        }
    }
    for (auto& var : result.Variables) {
        const auto* srcType = var.Type;
        auto it = typeIndexCache.find(srcType);
        RADRAY_ASSERT(it != typeIndexCache.end());
        var.Type = &result.Types[it->second];
    }

    return result;
}

}  // namespace radray::render

#ifdef RADRAY_ENABLE_DXC

#ifdef RADRAY_PLATFORM_WINDOWS
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
using Microsoft::WRL::ComPtr;
#include <radray/platform.h>
#include <directx/d3d12shader.h>
#ifdef VOID
#undef VOID
#endif
#else
// TODO:
#endif

#include <dxcapi.h>

#include <radray/errors.h>
#include <radray/logger.h>
#include <radray/utility.h>

namespace radray::render {

static HlslCBufferType _MapCBufferType(D3D_CBUFFER_TYPE type) noexcept {
    switch (type) {
        case D3D_CT_CBUFFER: return HlslCBufferType::CBUFFER;
        case D3D_CT_TBUFFER: return HlslCBufferType::TBUFFER;
        case D3D_CT_INTERFACE_POINTERS: return HlslCBufferType::INTERFACE_POINTERS;
        case D3D_CT_RESOURCE_BIND_INFO: return HlslCBufferType::RESOURCE_BIND_INFO;
        default: return HlslCBufferType::UNKNOWN;
    }
}

static HlslShaderInputType _MapInputType(D3D_SHADER_INPUT_TYPE type) noexcept {
    switch (type) {
        case D3D_SIT_CBUFFER: return HlslShaderInputType::CBUFFER;
        case D3D_SIT_TBUFFER: return HlslShaderInputType::TBUFFER;
        case D3D_SIT_TEXTURE: return HlslShaderInputType::TEXTURE;
        case D3D_SIT_SAMPLER: return HlslShaderInputType::SAMPLER;
        case D3D_SIT_STRUCTURED: return HlslShaderInputType::STRUCTURED;
        case D3D_SIT_BYTEADDRESS: return HlslShaderInputType::BYTEADDRESS;
        case D3D_SIT_UAV_RWTYPED: return HlslShaderInputType::UAV_RWTYPED;
        case D3D_SIT_UAV_RWSTRUCTURED: return HlslShaderInputType::UAV_RWSTRUCTURED;
        case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER: return HlslShaderInputType::UAV_RWSTRUCTURED_WITH_COUNTER;
        case D3D_SIT_UAV_APPEND_STRUCTURED: return HlslShaderInputType::UAV_APPEND_STRUCTURED;
        case D3D_SIT_UAV_CONSUME_STRUCTURED: return HlslShaderInputType::UAV_CONSUME_STRUCTURED;
        case D3D_SIT_UAV_RWBYTEADDRESS: return HlslShaderInputType::UAV_RWBYTEADDRESS;
        case D3D_SIT_RTACCELERATIONSTRUCTURE: return HlslShaderInputType::RTACCELERATIONSTRUCTURE;
        case D3D_SIT_UAV_FEEDBACKTEXTURE: return HlslShaderInputType::UAV_FEEDBACKTEXTURE;
        default: return HlslShaderInputType::UNKNOWN;
    }
}

static HlslResourceReturnType _MapReturnType(D3D_RESOURCE_RETURN_TYPE type) noexcept {
    switch (type) {
        case D3D_RETURN_TYPE_UNORM: return HlslResourceReturnType::UNORM;
        case D3D_RETURN_TYPE_SNORM: return HlslResourceReturnType::SNORM;
        case D3D_RETURN_TYPE_SINT: return HlslResourceReturnType::SINT;
        case D3D_RETURN_TYPE_UINT: return HlslResourceReturnType::UINT;
        case D3D_RETURN_TYPE_FLOAT: return HlslResourceReturnType::FLOAT;
        case D3D_RETURN_TYPE_MIXED: return HlslResourceReturnType::MIXED;
        case D3D_RETURN_TYPE_DOUBLE: return HlslResourceReturnType::DOUBLE;
        case D3D_RETURN_TYPE_CONTINUED: return HlslResourceReturnType::CONTINUED;
        default: return HlslResourceReturnType::UNKNOWN;
    }
}

static HlslSRVDimension _MapSRVDimension(D3D_SRV_DIMENSION dim) noexcept {
    switch (dim) {
        case D3D_SRV_DIMENSION_BUFFER: return HlslSRVDimension::BUFFER;
        case D3D_SRV_DIMENSION_TEXTURE1D: return HlslSRVDimension::TEXTURE1D;
        case D3D_SRV_DIMENSION_TEXTURE1DARRAY: return HlslSRVDimension::TEXTURE1DARRAY;
        case D3D_SRV_DIMENSION_TEXTURE2D: return HlslSRVDimension::TEXTURE2D;
        case D3D_SRV_DIMENSION_TEXTURE2DARRAY: return HlslSRVDimension::TEXTURE2DARRAY;
        case D3D_SRV_DIMENSION_TEXTURE2DMS: return HlslSRVDimension::TEXTURE2DMS;
        case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY: return HlslSRVDimension::TEXTURE2DMSARRAY;
        case D3D_SRV_DIMENSION_TEXTURE3D: return HlslSRVDimension::TEXTURE3D;
        case D3D_SRV_DIMENSION_TEXTURECUBE: return HlslSRVDimension::TEXTURECUBE;
        case D3D_SRV_DIMENSION_TEXTURECUBEARRAY: return HlslSRVDimension::TEXTURECUBEARRAY;
        case D3D_SRV_DIMENSION_BUFFEREX: return HlslSRVDimension::BUFFEREX;
        default: return HlslSRVDimension::UNKNOWN;
    }
}

static HlslSystemValueType _MapSystemValue(D3D_NAME name) noexcept {
    switch (name) {
        case D3D_NAME_POSITION: return HlslSystemValueType::POSITION;
        case D3D_NAME_CLIP_DISTANCE: return HlslSystemValueType::CLIP_DISTANCE;
        case D3D_NAME_CULL_DISTANCE: return HlslSystemValueType::CULL_DISTANCE;
        case D3D_NAME_RENDER_TARGET_ARRAY_INDEX: return HlslSystemValueType::RENDER_TARGET_ARRAY_INDEX;
        case D3D_NAME_VIEWPORT_ARRAY_INDEX: return HlslSystemValueType::VIEWPORT_ARRAY_INDEX;
        case D3D_NAME_VERTEX_ID: return HlslSystemValueType::VERTEX_ID;
        case D3D_NAME_PRIMITIVE_ID: return HlslSystemValueType::PRIMITIVE_ID;
        case D3D_NAME_INSTANCE_ID: return HlslSystemValueType::INSTANCE_ID;
        case D3D_NAME_IS_FRONT_FACE: return HlslSystemValueType::IS_FRONT_FACE;
        case D3D_NAME_SAMPLE_INDEX: return HlslSystemValueType::SAMPLE_INDEX;
        case D3D_NAME_FINAL_QUAD_EDGE_TESSFACTOR: return HlslSystemValueType::FINAL_QUAD_EDGE_TESSFACTOR;
        case D3D_NAME_FINAL_QUAD_INSIDE_TESSFACTOR: return HlslSystemValueType::FINAL_QUAD_INSIDE_TESSFACTOR;
        case D3D_NAME_FINAL_TRI_EDGE_TESSFACTOR: return HlslSystemValueType::FINAL_TRI_EDGE_TESSFACTOR;
        case D3D_NAME_FINAL_TRI_INSIDE_TESSFACTOR: return HlslSystemValueType::FINAL_TRI_INSIDE_TESSFACTOR;
        case D3D_NAME_FINAL_LINE_DETAIL_TESSFACTOR: return HlslSystemValueType::FINAL_LINE_DETAIL_TESSFACTOR;
        case D3D_NAME_FINAL_LINE_DENSITY_TESSFACTOR: return HlslSystemValueType::FINAL_LINE_DENSITY_TESSFACTOR;
        case D3D_NAME_BARYCENTRICS: return HlslSystemValueType::BARYCENTRICS;
        case D3D_NAME_SHADINGRATE: return HlslSystemValueType::SHADINGRATE;
        case D3D_NAME_CULLPRIMITIVE: return HlslSystemValueType::CULLPRIMITIVE;
        case D3D_NAME_TARGET: return HlslSystemValueType::TARGET;
        case D3D_NAME_DEPTH: return HlslSystemValueType::DEPTH;
        case D3D_NAME_COVERAGE: return HlslSystemValueType::COVERAGE;
        case D3D_NAME_DEPTH_GREATER_EQUAL: return HlslSystemValueType::DEPTH_GREATER_EQUAL;
        case D3D_NAME_DEPTH_LESS_EQUAL: return HlslSystemValueType::DEPTH_LESS_EQUAL;
        case D3D_NAME_STENCIL_REF: return HlslSystemValueType::STENCIL_REF;
        case D3D_NAME_INNER_COVERAGE: return HlslSystemValueType::INNER_COVERAGE;
        default: return HlslSystemValueType::UNDEFINED;
    }
}

static HlslRegisterComponentType _MapComponentType(D3D_REGISTER_COMPONENT_TYPE type) noexcept {
    switch (type) {
        case D3D_REGISTER_COMPONENT_UINT32: return HlslRegisterComponentType::UINT32;
        case D3D_REGISTER_COMPONENT_SINT32: return HlslRegisterComponentType::SINT32;
        case D3D_REGISTER_COMPONENT_FLOAT32: return HlslRegisterComponentType::FLOAT32;
        case D3D_REGISTER_COMPONENT_UINT16: return HlslRegisterComponentType::UINT16;
        case D3D_REGISTER_COMPONENT_SINT16: return HlslRegisterComponentType::SINT16;
        case D3D_REGISTER_COMPONENT_FLOAT16: return HlslRegisterComponentType::FLOAT16;
        case D3D_REGISTER_COMPONENT_UINT64: return HlslRegisterComponentType::UINT64;
        case D3D_REGISTER_COMPONENT_SINT64: return HlslRegisterComponentType::SINT64;
        case D3D_REGISTER_COMPONENT_FLOAT64: return HlslRegisterComponentType::FLOAT64;
        default: return HlslRegisterComponentType::UNKNOWN;
    }
}

static HlslFeatureLevel _MapFeatureLevel(D3D_FEATURE_LEVEL level) noexcept {
    switch (level) {
        case D3D_FEATURE_LEVEL_9_1: return HlslFeatureLevel::LEVEL9_1;
        case D3D_FEATURE_LEVEL_9_2: return HlslFeatureLevel::LEVEL9_2;
        case D3D_FEATURE_LEVEL_9_3: return HlslFeatureLevel::LEVEL9_3;
        case D3D_FEATURE_LEVEL_10_0: return HlslFeatureLevel::LEVEL10_0;
        case D3D_FEATURE_LEVEL_10_1: return HlslFeatureLevel::LEVEL10_1;
        case D3D_FEATURE_LEVEL_11_0: return HlslFeatureLevel::LEVEL11_0;
        case D3D_FEATURE_LEVEL_11_1: return HlslFeatureLevel::LEVEL11_1;
        case D3D_FEATURE_LEVEL_12_0: return HlslFeatureLevel::LEVEL12_0;
        case D3D_FEATURE_LEVEL_12_1: return HlslFeatureLevel::LEVEL12_1;
        case D3D_FEATURE_LEVEL_12_2: return HlslFeatureLevel::LEVEL12_2;
        default: return HlslFeatureLevel::UNKNOWN;
    }
}

static HlslShaderVariableClass _MapShaderVariableClass(D3D_SHADER_VARIABLE_CLASS cls) noexcept {
    switch (cls) {
        case D3D_SVC_SCALAR: return HlslShaderVariableClass::SCALAR;
        case D3D_SVC_VECTOR: return HlslShaderVariableClass::VECTOR;
        case D3D_SVC_MATRIX_ROWS: return HlslShaderVariableClass::MATRIX_ROWS;
        case D3D_SVC_MATRIX_COLUMNS: return HlslShaderVariableClass::MATRIX_COLUMNS;
        case D3D_SVC_OBJECT: return HlslShaderVariableClass::OBJECT;
        case D3D_SVC_STRUCT: return HlslShaderVariableClass::STRUCTURE;
        case D3D_SVC_INTERFACE_CLASS: return HlslShaderVariableClass::OBJECT;
        case D3D_SVC_INTERFACE_POINTER: return HlslShaderVariableClass::OBJECT;
        default: return HlslShaderVariableClass::UNKNOWN;
    }
}

static HlslShaderVariableType _MapShaderVariableType(D3D_SHADER_VARIABLE_TYPE type) noexcept {
    switch (type) {
        case D3D_SVT_VOID: return HlslShaderVariableType::VOID;
        case D3D_SVT_BOOL: return HlslShaderVariableType::BOOL;
        case D3D_SVT_INT: return HlslShaderVariableType::INT;
        case D3D_SVT_FLOAT: return HlslShaderVariableType::FLOAT;
        case D3D_SVT_STRING: return HlslShaderVariableType::STRING;
        case D3D_SVT_TEXTURE: return HlslShaderVariableType::TEXTURE;
        case D3D_SVT_TEXTURE1D: return HlslShaderVariableType::TEXTURE1D;
        case D3D_SVT_TEXTURE2D: return HlslShaderVariableType::TEXTURE2D;
        case D3D_SVT_TEXTURE3D: return HlslShaderVariableType::TEXTURE3D;
        case D3D_SVT_TEXTURECUBE: return HlslShaderVariableType::TEXTURECUBE;
        case D3D_SVT_SAMPLER: return HlslShaderVariableType::SAMPLER;
        case D3D_SVT_SAMPLER1D: return HlslShaderVariableType::SAMPLER1D;
        case D3D_SVT_SAMPLER2D: return HlslShaderVariableType::SAMPLER2D;
        case D3D_SVT_SAMPLER3D: return HlslShaderVariableType::SAMPLER3D;
        case D3D_SVT_SAMPLERCUBE: return HlslShaderVariableType::SAMPLERCUBE;
        case D3D_SVT_PIXELSHADER: return HlslShaderVariableType::PIXELSHADER;
        case D3D_SVT_VERTEXSHADER: return HlslShaderVariableType::VERTEXSHADER;
        case D3D_SVT_PIXELFRAGMENT: return HlslShaderVariableType::PIXELFRAGMENT;
        case D3D_SVT_VERTEXFRAGMENT: return HlslShaderVariableType::VERTEXFRAGMENT;
        case D3D_SVT_UINT: return HlslShaderVariableType::UINT;
        case D3D_SVT_UINT8: return HlslShaderVariableType::UINT8;
        case D3D_SVT_GEOMETRYSHADER: return HlslShaderVariableType::GEOMETRYSHADER;
        case D3D_SVT_RASTERIZER: return HlslShaderVariableType::RASTERIZER;
        case D3D_SVT_DEPTHSTENCIL: return HlslShaderVariableType::DEPTHSTENCIL;
        case D3D_SVT_BLEND: return HlslShaderVariableType::BLEND;
        case D3D_SVT_BUFFER: return HlslShaderVariableType::BUFFER;
        case D3D_SVT_CBUFFER: return HlslShaderVariableType::CBUFFER;
        case D3D_SVT_TBUFFER: return HlslShaderVariableType::TBUFFER;
        case D3D_SVT_TEXTURE1DARRAY: return HlslShaderVariableType::TEXTURE1DARRAY;
        case D3D_SVT_TEXTURE2DARRAY: return HlslShaderVariableType::TEXTURE2DARRAY;
        case D3D_SVT_RENDERTARGETVIEW: return HlslShaderVariableType::RENDERTARGETVIEW;
        case D3D_SVT_DEPTHSTENCILVIEW: return HlslShaderVariableType::DEPTHSTENCILVIEW;
        case D3D_SVT_TEXTURE2DMS: return HlslShaderVariableType::TEXTURE2DMS;
        case D3D_SVT_TEXTURE2DMSARRAY: return HlslShaderVariableType::TEXTURE2DMSARRAY;
        case D3D_SVT_TEXTURECUBEARRAY: return HlslShaderVariableType::TEXTURECUBEARRAY;
        case D3D_SVT_HULLSHADER: return HlslShaderVariableType::HULLSHADER;
        case D3D_SVT_DOMAINSHADER: return HlslShaderVariableType::DOMAINSHADER;
        case D3D_SVT_INTERFACE_POINTER: return HlslShaderVariableType::INTERFACE_POINTER;
        case D3D_SVT_COMPUTESHADER: return HlslShaderVariableType::COMPUTESHADER;
        case D3D_SVT_DOUBLE: return HlslShaderVariableType::DOUBLE;
        case D3D_SVT_RWTEXTURE1D: return HlslShaderVariableType::RWTEXTURE1D;
        case D3D_SVT_RWTEXTURE1DARRAY: return HlslShaderVariableType::RWTEXTURE1DARRAY;
        case D3D_SVT_RWTEXTURE2D: return HlslShaderVariableType::RWTEXTURE2D;
        case D3D_SVT_RWTEXTURE2DARRAY: return HlslShaderVariableType::RWTEXTURE2DARRAY;
        case D3D_SVT_RWTEXTURE3D: return HlslShaderVariableType::RWTEXTURE3D;
        case D3D_SVT_RWBUFFER: return HlslShaderVariableType::RWBUFFER;
        case D3D_SVT_BYTEADDRESS_BUFFER: return HlslShaderVariableType::BYTEADDRESS_BUFFER;
        case D3D_SVT_RWBYTEADDRESS_BUFFER: return HlslShaderVariableType::RWBYTEADDRESS_BUFFER;
        case D3D_SVT_STRUCTURED_BUFFER: return HlslShaderVariableType::STRUCTURED_BUFFER;
        case D3D_SVT_RWSTRUCTURED_BUFFER: return HlslShaderVariableType::RWSTRUCTURED_BUFFER;
        case D3D_SVT_APPEND_STRUCTURED_BUFFER: return HlslShaderVariableType::APPEND_STRUCTURED_BUFFER;
        case D3D_SVT_CONSUME_STRUCTURED_BUFFER: return HlslShaderVariableType::CONSUME_STRUCTURED_BUFFER;
        case D3D_SVT_MIN8FLOAT: return HlslShaderVariableType::MIN8FLOAT;
        case D3D_SVT_MIN10FLOAT: return HlslShaderVariableType::MIN10FLOAT;
        case D3D_SVT_MIN16FLOAT: return HlslShaderVariableType::MIN16FLOAT;
        case D3D_SVT_MIN12INT: return HlslShaderVariableType::MIN12INT;
        case D3D_SVT_MIN16INT: return HlslShaderVariableType::MIN16INT;
        case D3D_SVT_MIN16UINT: return HlslShaderVariableType::MIN16UINT;
        case D3D_SVT_INT16: return HlslShaderVariableType::INT16;
        case D3D_SVT_UINT16: return HlslShaderVariableType::UINT16;
        case D3D_SVT_FLOAT16: return HlslShaderVariableType::FLOAT16;
        case D3D_SVT_INT64: return HlslShaderVariableType::INT64;
        case D3D_SVT_UINT64: return HlslShaderVariableType::UINT64;
        default: return HlslShaderVariableType::UNKNOWN;
    }
}

#ifdef RADRAY_ENABLE_MIMALLOC
class MiMallocAdapter : public IMalloc {
public:
    virtual ~MiMallocAdapter() noexcept = default;

    HRESULT QueryInterface(REFIID riid, void** ppvObject) override {
        if (riid == __uuidof(IMalloc)) {
            *ppvObject = static_cast<IMalloc*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG AddRef() override {
        return ++_refCnt;
    }
    ULONG Release() override {
        auto count = --_refCnt;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    void* Alloc(SIZE_T cb) override { return mi_malloc(cb); }
    void* Realloc(void* pv, SIZE_T cb) override { return mi_realloc(pv, cb); }
    void Free(void* pv) override { mi_free(pv); }
    SIZE_T GetSize(void* pv) override { return mi_usable_size(pv); }
    int DidAlloc(void* pv) override { return pv == nullptr ? -1 : (mi_is_in_heap_region(pv) ? 1 : 0); }
    void HeapMinimize() override { mi_collect(true); }

private:
    std::atomic<ULONG> _refCnt;
};
#endif

class DxcImpl : public Dxc::Impl, public Noncopyable {
public:
    class ArgsData {
    public:
        ShaderBlobCategory category{};
        bool isSpirv{false};
        bool isMetal{false};
        bool isStripRefl{false};
    };

    class CompileStateData {
    public:
        HRESULT Status;
        std::string_view ErrMsg;
    };

#ifdef RADRAY_ENABLE_MIMALLOC
    DxcImpl(
        ComPtr<MiMallocAdapter> mi,
        ComPtr<IDxcCompiler3> dxc,
        ComPtr<IDxcUtils> utils,
        ComPtr<IDxcIncludeHandler> inc) noexcept
        : _mi(std::move(mi)),
          _dxc(std::move(dxc)),
          _utils(std::move(utils)),
          _inc(std::move(inc)) {}
#else
    DxcImpl(
        ComPtr<IDxcCompiler3> dxc,
        ComPtr<IDxcUtils> utils,
        ComPtr<IDxcIncludeHandler> inc) noexcept
        : _dxc(std::move(dxc)),
          _utils(std::move(utils)),
          _inc(std::move(inc)) {}
#endif
    ~DxcImpl() noexcept override = default;

    ArgsData ParseArgs(std::span<std::string_view> args) noexcept {
        ArgsData result{};
        for (auto i : args) {
            if (i == "-spirv") {
                result.isSpirv = true;
            }
            if (i == "-metal") {
                result.isMetal = true;
            }
            if (i == "-Qstrip_reflect") {
                result.isStripRefl = true;
            }
        }
        result.category = result.isSpirv ? ShaderBlobCategory::SPIRV : (result.isMetal ? ShaderBlobCategory::MSL : ShaderBlobCategory::DXIL);
        return result;
    }

    Nullable<ComPtr<IDxcResult>> CompileImpl(std::string_view code, std::span<std::string_view> args) noexcept {
        vector<wstring> wargs;
        wargs.reserve(args.size());
        for (auto i : args) {
            auto w = ToWideChar(i);
            if (!w.has_value()) {
                RADRAY_ERR_LOG("{} {}", Errors::DXC, i);
                return Nullable<ComPtr<IDxcResult>>{nullptr};
            }
            wargs.emplace_back(std::move(w.value()));
        }
        vector<LPCWSTR> argsref;
        argsref.reserve(wargs.size());
        for (auto&& i : wargs) {
            argsref.emplace_back(i.c_str());
        }
        DxcBuffer buffer{code.data(), code.size(), CP_ACP};
        ComPtr<IDxcResult> compileResult;
        if (HRESULT hr = _dxc->Compile(
                &buffer,
                argsref.data(),
                (UINT32)argsref.size(),
                _inc.Get(),
                IID_PPV_ARGS(&compileResult));
            FAILED(hr)) {
            RADRAY_ERR_LOG("{} {}", Errors::DXC, hr);
            return Nullable<ComPtr<IDxcResult>>{nullptr};
        }
        return Nullable<ComPtr<IDxcResult>>{std::move(compileResult)};
    }

    CompileStateData GetCompileState(IDxcResult* compileResult) noexcept {
        CompileStateData result{};
        if (HRESULT hr = compileResult->GetStatus(&result.Status);
            FAILED(hr)) {
            RADRAY_ERR_LOG("{} {}", Errors::DXC, hr);
            result.Status = hr;
            return result;
        }
        ComPtr<IDxcBlobEncoding> errBuffer;
        if (compileResult->HasOutput(DXC_OUT_ERRORS)) {
            if (HRESULT hr = compileResult->GetErrorBuffer(&errBuffer);
                FAILED(hr)) {
                RADRAY_ERR_LOG("{} {}", Errors::DXC, hr);
                errBuffer = nullptr;
            }
        }
        if (errBuffer) {
            result.ErrMsg = {std::bit_cast<char const*>(errBuffer->GetBufferPointer()), errBuffer->GetBufferSize()};
        }
        return result;
    }

    std::span<const byte> GetResultData(IDxcResult* compileResult) noexcept {
        ComPtr<IDxcBlob> blob;
        if (HRESULT hr = compileResult->GetResult(&blob);
            FAILED(hr)) {
            RADRAY_ERR_LOG("{} {}", Errors::DXC, hr);
            return {};
        }
        auto blobStart = std::bit_cast<byte const*>(blob->GetBufferPointer());
        return {blobStart, blob->GetBufferSize()};
    }

    std::span<const byte> GetBlobData(IDxcResult* compileResult, DXC_OUT_KIND kind) noexcept {
        ComPtr<IDxcBlob> blob;
        if (HRESULT hr = compileResult->GetOutput(kind, IID_PPV_ARGS(&blob), nullptr);
            FAILED(hr)) {
            RADRAY_ERR_LOG("{} {}", Errors::DXC, hr);
            return {};
        }
        auto reflStart = std::bit_cast<byte const*>(blob->GetBufferPointer());
        return {reflStart, reflStart + blob->GetBufferSize()};
    }

    std::optional<DxcOutput> Compile(std::string_view code, std::span<std::string_view> args) noexcept {
        ComPtr<IDxcResult> compileResult;
        {
            auto compileResultOpt = CompileImpl(code, args);
            if (!compileResultOpt.HasValue()) {
                return std::nullopt;
            }
            compileResult = compileResultOpt.Unwrap();
        }
        auto [status, errMsg] = GetCompileState(compileResult.Get());
        if (!errMsg.empty()) {
            RADRAY_ERR_LOG("{} {}", Errors::DXC, "compile message");
            RADRAY_ERR_LOG("{}", errMsg);
        }
        if (FAILED(status)) {
            return std::nullopt;
        }
        std::span<const byte> resultData = GetResultData(compileResult.Get());
        if (resultData.empty()) {
            return std::nullopt;
        }
        std::span<const byte> reflData{};
        if (compileResult->HasOutput(DXC_OUT_REFLECTION)) {
            reflData = GetBlobData(compileResult.Get(), DXC_OUT_REFLECTION);
        }
        std::span<const byte> radrayReflData{};
        if (compileResult->HasOutput(DXC_OUT_REFLECTION_RADRAY)) {
            radrayReflData = GetBlobData(compileResult.Get(), DXC_OUT_REFLECTION_RADRAY);
        } else {
            RADRAY_ERR_LOG("{} {}", Errors::DXC, "no radray ext reflection data");
            return std::nullopt;
        }
        auto reflExtOpt = DeserializeDxcReflectionRadrayExt(radrayReflData);
        if (!reflExtOpt.has_value()) {
            RADRAY_ERR_LOG("{} {}", Errors::DXC, "no radray ext reflection data");
            return std::nullopt;
        }
        auto argsData = ParseArgs(args);
        return DxcOutput{
            {resultData.begin(), resultData.end()},
            {reflData.begin(), reflData.end()},
            std::move(reflExtOpt.value()),
            argsData.category};
    }

    std::optional<HlslShaderDesc> GetShaderDescFromOutput(ShaderStage stage, std::span<const byte> refl) noexcept {
        if (refl.empty()) {
            return std::nullopt;
        }
        DxcBuffer buf{refl.data(), refl.size(), 0};
        ComPtr<ID3D12ShaderReflection> sr;
        if (HRESULT hr = _utils->CreateReflection(&buf, IID_PPV_ARGS(&sr));
            FAILED(hr)) {
            RADRAY_ERR_LOG("{} {}::{} {}", Errors::DXC, "IDxcUtils", "CreateReflection", hr);
            return std::nullopt;
        }
        D3D12_SHADER_DESC shaderDesc{};
        if (HRESULT hr = sr->GetDesc(&shaderDesc);
            FAILED(hr)) {
            RADRAY_ERR_LOG("{} {}::{} {}", Errors::DXC, "ID3D12ShaderReflection", "GetDesc", hr);
            return std::nullopt;
        }
        HlslShaderDesc desc{};
        const auto d3dStage = static_cast<D3D12_SHADER_VERSION_TYPE>(D3D12_SHVER_GET_TYPE(shaderDesc.Version));
        switch (d3dStage) {
            case D3D12_SHVER_PIXEL_SHADER: desc.Stage = ShaderStage::Pixel; break;
            case D3D12_SHVER_VERTEX_SHADER: desc.Stage = ShaderStage::Vertex; break;
            case D3D12_SHVER_COMPUTE_SHADER: desc.Stage = ShaderStage::Compute; break;
            default: {
                RADRAY_ERR_LOG("{} {}", Errors::DXC, "unsupported shader stage");
                return std::nullopt;
            }
        }
        if (shaderDesc.Creator != nullptr) {
            desc.Creator = shaderDesc.Creator;
        }
        desc.Version = shaderDesc.Version;
        desc.Flags = shaderDesc.Flags;
        D3D_FEATURE_LEVEL featureLevel{};
        if (SUCCEEDED(sr->GetMinFeatureLevel(&featureLevel))) {
            desc.MinFeatureLevel = _MapFeatureLevel(featureLevel);
        }
        if (stage == ShaderStage::Compute) {
            UINT groupX{}, groupY{}, groupZ{};
            sr->GetThreadGroupSize(&groupX, &groupY, &groupZ);
            desc.GroupSizeX = groupX;
            desc.GroupSizeY = groupY;
            desc.GroupSizeZ = groupZ;
        }
        constexpr size_t invalidTypeIndex = std::numeric_limits<size_t>::max();
        unordered_map<ID3D12ShaderReflectionType*, size_t> typeIndexCache;
        struct TypeMemberFix {
            string Name;
            size_t TypeIndex;
        };
        vector<vector<TypeMemberFix>> typeMemberFixups;
        struct VariableTypeFix {
            size_t variableIndex;
            size_t typeIndex;
        };
        vector<VariableTypeFix> pendingVariableTypes;
        vector<vector<size_t>> cbufferVariableIndices;
        cbufferVariableIndices.reserve(shaderDesc.ConstantBuffers);
        std::function<size_t(ID3D12ShaderReflectionType*)> getOrCreateTypeIndex = [&](ID3D12ShaderReflectionType* typeRefl) -> size_t {
            if (typeRefl == nullptr) {
                return invalidTypeIndex;
            }
            if (auto it = typeIndexCache.find(typeRefl); it != typeIndexCache.end()) {
                return it->second;
            }
            D3D12_SHADER_TYPE_DESC typeDesc{};
            if (HRESULT hr = typeRefl->GetDesc(&typeDesc);
                FAILED(hr)) {
                RADRAY_ERR_LOG("{} {}::{} {}", Errors::DXC, "ID3D12ShaderReflectionType", "GetDesc", hr);
                return invalidTypeIndex;
            }
            size_t newIndex = desc.Types.size();
            typeIndexCache.emplace(typeRefl, newIndex);
            desc.Types.emplace_back();
            typeMemberFixups.emplace_back();
            auto& storedType = desc.Types.back();
            if (typeDesc.Name != nullptr) {
                storedType.Name = typeDesc.Name;
            }
            storedType.Class = _MapShaderVariableClass(typeDesc.Class);
            storedType.Type = _MapShaderVariableType(typeDesc.Type);
            storedType.Rows = typeDesc.Rows;
            storedType.Columns = typeDesc.Columns;
            storedType.Elements = typeDesc.Elements;
            storedType.Offset = typeDesc.Offset;
            size_t pendingMemberListIndex = typeMemberFixups.size() - 1;
            typeMemberFixups[pendingMemberListIndex].reserve(typeDesc.Members);
            for (UINT memberIdx = 0; memberIdx < typeDesc.Members; memberIdx++) {
                auto* memberType = typeRefl->GetMemberTypeByIndex(memberIdx);
                size_t memberTypeIndex = getOrCreateTypeIndex(memberType);
                string memberName;
                if (auto* name = typeRefl->GetMemberTypeName(memberIdx); name != nullptr) {
                    memberName = name;
                }
                if (memberTypeIndex != invalidTypeIndex) {
                    typeMemberFixups[pendingMemberListIndex].push_back(TypeMemberFix{
                        .Name = std::move(memberName),
                        .TypeIndex = memberTypeIndex,
                    });
                }
            }
            return newIndex;
        };
        desc.ConstantBuffers.reserve(shaderDesc.ConstantBuffers);
        for (UINT i = 0; i < shaderDesc.ConstantBuffers; i++) {
            auto* cb = sr->GetConstantBufferByIndex(i);
            if (cb == nullptr) {
                RADRAY_ERR_LOG("{} {}::{} {}", Errors::DXC, "ID3D12ShaderReflection", "GetConstantBufferByIndex", i);
                continue;
            }
            D3D12_SHADER_BUFFER_DESC cbDesc{};
            if (HRESULT hr = cb->GetDesc(&cbDesc);
                FAILED(hr)) {
                RADRAY_ERR_LOG("{} {}::{} {}", Errors::DXC, "ID3D12ShaderReflectionConstantBuffer", "GetDesc", hr);
                continue;
            }
            HlslShaderBufferDesc bufferDesc{};
            if (cbDesc.Name != nullptr) {
                bufferDesc.Name = cbDesc.Name;
            }
            bufferDesc.Type = _MapCBufferType(cbDesc.Type);
            bufferDesc.Size = cbDesc.Size;
            bufferDesc.Flags = cbDesc.uFlags;
            desc.ConstantBuffers.emplace_back(std::move(bufferDesc));
            cbufferVariableIndices.emplace_back();
            auto& bufferVariableIndices = cbufferVariableIndices.back();
            bufferVariableIndices.reserve(cbDesc.Variables);
            for (UINT j = 0; j < cbDesc.Variables; j++) {
                auto* var = cb->GetVariableByIndex(j);
                if (var == nullptr) {
                    RADRAY_ERR_LOG("{} {}::{} {}", Errors::DXC, "ID3D12ShaderReflectionConstantBuffer", "GetVariableByIndex", j);
                    continue;
                }
                D3D12_SHADER_VARIABLE_DESC varDesc{};
                if (HRESULT hr = var->GetDesc(&varDesc);
                    FAILED(hr)) {
                    RADRAY_ERR_LOG("{} {}::{} {}", Errors::DXC, "ID3D12ShaderReflectionVariable", "GetDesc", hr);
                    continue;
                }
                HlslShaderVariableDesc hlslVar{};
                if (varDesc.Name != nullptr) {
                    hlslVar.Name = varDesc.Name;
                }
                hlslVar.StartOffset = varDesc.StartOffset;
                hlslVar.Size = varDesc.Size;
                hlslVar.uFlags = varDesc.uFlags;
                hlslVar.StartTexture = varDesc.StartTexture;
                hlslVar.TextureSize = varDesc.TextureSize;
                hlslVar.StartSampler = varDesc.StartSampler;
                hlslVar.SamplerSize = varDesc.SamplerSize;
                size_t variableIndex = desc.Variables.size();
                desc.Variables.emplace_back(std::move(hlslVar));
                bufferVariableIndices.emplace_back(variableIndex);
                auto* typeRefl = var->GetType();
                size_t typeIndex = getOrCreateTypeIndex(typeRefl);
                if (typeIndex != invalidTypeIndex) {
                    pendingVariableTypes.push_back({variableIndex, typeIndex});
                }
            }
        }
        desc.BoundResources.reserve(shaderDesc.BoundResources);
        for (UINT i = 0; i < shaderDesc.BoundResources; i++) {
            D3D12_SHADER_INPUT_BIND_DESC bindDesc{};
            if (HRESULT hr = sr->GetResourceBindingDesc(i, &bindDesc);
                FAILED(hr)) {
                RADRAY_ERR_LOG("{} {}::{} {}", Errors::DXC, "ID3D12ShaderReflection", "GetResourceBindingDesc", hr);
                continue;
            }
            HlslInputBindDesc bind{};
            if (bindDesc.Name != nullptr) {
                bind.Name = bindDesc.Name;
            }
            bind.Type = _MapInputType(bindDesc.Type);
            bind.BindPoint = bindDesc.BindPoint;
            bind.BindCount = bindDesc.BindCount;
            bind.Flags = bindDesc.uFlags;
            bind.ReturnType = _MapReturnType(bindDesc.ReturnType);
            bind.Dimension = _MapSRVDimension(bindDesc.Dimension);
            bind.NumSamples = bindDesc.NumSamples;
            bind.Space = bindDesc.Space;
            desc.BoundResources.emplace_back(std::move(bind));
        }
        desc.InputParameters.reserve(shaderDesc.InputParameters);
        for (UINT i = 0; i < shaderDesc.InputParameters; i++) {
            D3D12_SIGNATURE_PARAMETER_DESC paramDesc{};
            if (HRESULT hr = sr->GetInputParameterDesc(i, &paramDesc);
                FAILED(hr)) {
                RADRAY_ERR_LOG("{} {}::{} {}", Errors::DXC, "ID3D12ShaderReflection", "GetInputParameterDesc", hr);
                continue;
            }
            HlslSignatureParameterDesc param{};
            if (paramDesc.SemanticName != nullptr) {
                param.SemanticName = paramDesc.SemanticName;
            }
            param.SemanticIndex = paramDesc.SemanticIndex;
            param.Register = paramDesc.Register;
            param.SystemValueType = _MapSystemValue(paramDesc.SystemValueType);
            param.ComponentType = _MapComponentType(paramDesc.ComponentType);
            param.Stream = paramDesc.Stream;
            desc.InputParameters.emplace_back(std::move(param));
        }
        desc.OutputParameters.reserve(shaderDesc.OutputParameters);
        for (UINT i = 0; i < shaderDesc.OutputParameters; i++) {
            D3D12_SIGNATURE_PARAMETER_DESC paramDesc{};
            if (HRESULT hr = sr->GetOutputParameterDesc(i, &paramDesc);
                FAILED(hr)) {
                RADRAY_ERR_LOG("{} {}::{} {}", Errors::DXC, "ID3D12ShaderReflection", "GetOutputParameterDesc", hr);
                continue;
            }
            HlslSignatureParameterDesc param{};
            if (paramDesc.SemanticName != nullptr) {
                param.SemanticName = paramDesc.SemanticName;
            }
            param.SemanticIndex = paramDesc.SemanticIndex;
            param.Register = paramDesc.Register;
            param.SystemValueType = _MapSystemValue(paramDesc.SystemValueType);
            param.ComponentType = _MapComponentType(paramDesc.ComponentType);
            param.Stream = paramDesc.Stream;
            desc.OutputParameters.emplace_back(std::move(param));
        }
        for (auto& fix : pendingVariableTypes) {
            if (fix.variableIndex < desc.Variables.size() && fix.typeIndex < desc.Types.size()) {
                desc.Variables[fix.variableIndex].Type = &desc.Types[fix.typeIndex];
            }
        }
        for (size_t i = 0; i < cbufferVariableIndices.size(); i++) {
            auto& bufferDesc = desc.ConstantBuffers[i];
            auto& indices = cbufferVariableIndices[i];
            bufferDesc.Variables.reserve(indices.size());
            for (auto varIndex : indices) {
                if (varIndex < desc.Variables.size()) {
                    bufferDesc.Variables.emplace_back(&desc.Variables[varIndex]);
                }
            }
            std::sort(bufferDesc.Variables.begin(), bufferDesc.Variables.end(), [](const auto* a, const auto* b) noexcept {
                return a->StartOffset < b->StartOffset;
            });
        }
        for (size_t i = 0; i < desc.Types.size(); i++) {
            auto& members = desc.Types[i].Members;
            auto& fixups = typeMemberFixups[i];
            members.reserve(fixups.size());
            for (auto& fix : fixups) {
                if (fix.TypeIndex < desc.Types.size()) {
                    HlslShaderTypeMember member{};
                    member.Name = fix.Name;
                    member.Type = &desc.Types[fix.TypeIndex];
                    members.emplace_back(std::move(member));
                }
            }
            std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) noexcept {
                return a.Type->Offset < b.Type->Offset;
            });
        }
        return desc;
    }

public:
#ifdef RADRAY_ENABLE_MIMALLOC
    ComPtr<MiMallocAdapter> _mi;
#endif
    ComPtr<IDxcCompiler3> _dxc;
    ComPtr<IDxcUtils> _utils;
    ComPtr<IDxcIncludeHandler> _inc;
};

Nullable<shared_ptr<Dxc>> CreateDxc() noexcept {
    ComPtr<IDxcCompiler3> dxc;
#if RADRAY_ENABLE_MIMALLOC
    ComPtr<MiMallocAdapter> mi{new MiMallocAdapter{}};
    if (HRESULT hr = DxcCreateInstance2(mi.Get(), CLSID_DxcCompiler, IID_PPV_ARGS(&dxc));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {} {}", Errors::DXC, "DxcCreateInstance2", hr);
        return nullptr;
    }
#else
    if (HRESULT hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {} {}", Errors::DXC, "DxcCreateInstance", hr);
        return nullptr;
    }
#endif
    ComPtr<IDxcUtils> utils;
    if (HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {} {}", Errors::DXC, "DxcCreateInstance", hr);
        return nullptr;
    }
    ComPtr<IDxcIncludeHandler> incHandler;
    if (HRESULT hr = utils->CreateDefaultIncludeHandler(&incHandler);
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {}", Errors::DXC, "IDxcUtils", "CreateDefaultIncludeHandler", hr);
        return nullptr;
    }
    auto implPtr = make_unique<DxcImpl>(
#ifdef RADRAY_ENABLE_MIMALLOC
        std::move(mi),
#endif
        std::move(dxc),
        std::move(utils),
        std::move(incHandler));
    return make_shared<Dxc>(std::move(implPtr));
}

void Dxc::Destroy() noexcept {
    _impl.reset();
}

std::optional<DxcOutput> Dxc::Compile(std::string_view code, std::span<std::string_view> args) noexcept {
    return static_cast<DxcImpl*>(_impl.get())->Compile(code, args);
}

std::optional<DxcOutput> Dxc::Compile(
    std::string_view code,
    std::string_view entryPoint,
    ShaderStage stage,
    HlslShaderModel sm,
    bool isOptimize,
    std::span<std::string_view> defines,
    std::span<std::string_view> includes,
    bool isSpirv) noexcept {
    string smStr = ([stage, sm]() noexcept {
        using oss = std::basic_ostringstream<char, std::char_traits<char>, allocator<char>>;
        oss s{};
        switch (stage) {
            case ShaderStage::Vertex: s << "vs_"; break;
            case ShaderStage::Pixel: s << "ps_"; break;
            case ShaderStage::Compute: s << "cs_"; break;
            default: s << "??_"; break;
        }
        switch (sm) {
            case HlslShaderModel::SM60: s << "6_0"; break;
            case HlslShaderModel::SM61: s << "6_1"; break;
            case HlslShaderModel::SM62: s << "6_2"; break;
            case HlslShaderModel::SM63: s << "6_3"; break;
            case HlslShaderModel::SM64: s << "6_4"; break;
            case HlslShaderModel::SM65: s << "6_5"; break;
            case HlslShaderModel::SM66: s << "6_6"; break;
        }
        string result = s.str();
        return result;
    })();
    vector<std::string_view> args{};
    if (isSpirv) {
        args.emplace_back("-spirv");
    }
    args.emplace_back("-all_resources_bound");
    {
        args.emplace_back("-HV");
        args.emplace_back("2021");
    }
    if (isOptimize) {
        args.emplace_back("-O3");
    } else {
        args.emplace_back("-Od");
        args.emplace_back("-Zi");
    }
    {
        args.emplace_back("-T");
        args.emplace_back(smStr);
    }
    {
        args.emplace_back("-E");
        args.emplace_back(entryPoint);
    }
    for (auto&& i : includes) {
        args.emplace_back("-I");
        args.emplace_back(i);
    }
    for (auto&& i : defines) {
        args.emplace_back("-D");
        args.emplace_back(i);
    }
    return static_cast<DxcImpl*>(_impl.get())->Compile(code, args);
}

std::optional<HlslShaderDesc> Dxc::GetShaderDescFromOutput(ShaderStage stage, std::span<const byte> refl) noexcept {
    return static_cast<DxcImpl*>(_impl.get())->GetShaderDescFromOutput(stage, refl);
}

}  // namespace radray::render

#endif
