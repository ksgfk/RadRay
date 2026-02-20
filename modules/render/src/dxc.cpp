#include <radray/render/dxc.h>

#include <utility>

#include <radray/utility.h>
#include <radray/text_encoding.h>

#ifdef RADRAY_ENABLE_MIMALLOC
#include <mimalloc.h>
#endif

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
    constexpr uint32_t Magic = 0x52524558;  // RREX
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

bool HlslInputBindDesc::IsUnboundArray() const noexcept {
    return BindCount == 0;
}

auto operator<=>(const HlslInputBindDesc& lhs, const HlslInputBindDesc& rhs) noexcept {
    if (auto cmp = lhs.Name <=> rhs.Name; cmp != 0) return cmp;
    if (auto cmp = lhs.Type <=> rhs.Type; cmp != 0) return cmp;
    if (auto cmp = lhs.BindPoint <=> rhs.BindPoint; cmp != 0) return cmp;
    if (auto cmp = lhs.BindCount <=> rhs.BindCount; cmp != 0) return cmp;
    if (auto cmp = lhs.ReturnType <=> rhs.ReturnType; cmp != 0) return cmp;
    if (auto cmp = lhs.Dimension <=> rhs.Dimension; cmp != 0) return cmp;
    if (auto cmp = lhs.NumSamples <=> rhs.NumSamples; cmp != 0) return cmp;
    if (auto cmp = lhs.Space <=> rhs.Space; cmp != 0) return cmp;
    if (auto cmp = lhs.Flags <=> rhs.Flags; cmp != 0) return cmp;
    return std::strong_ordering::equal;
}

bool operator==(const HlslInputBindDesc& lhs, const HlslInputBindDesc& rhs) noexcept {
    return (lhs <=> rhs) == 0;
}

bool operator!=(const HlslInputBindDesc& lhs, const HlslInputBindDesc& rhs) noexcept {
    return !(lhs == rhs);
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

bool IsHlslShaderBufferEqual(
    const HlslShaderDesc& l, const HlslShaderBufferDesc& lcb,
    const HlslShaderDesc& r, const HlslShaderBufferDesc& rcb) noexcept {
    if (lcb.Name != rcb.Name ||
        lcb.Type != rcb.Type ||
        lcb.Size != rcb.Size ||
        lcb.Flags != rcb.Flags ||
        lcb.Variables.size() != rcb.Variables.size()) {
        return false;
    }
    size_t len = lcb.Variables.size();
    for (size_t i = 0; i < len; i++) {
        if (lcb.Variables[i] >= l.Variables.size() || rcb.Variables[i] >= r.Variables.size()) {
            return false;
        }
        const auto& lVar = l.Variables[lcb.Variables[i]];
        const auto& rVar = r.Variables[rcb.Variables[i]];
        if (lVar.Name != rVar.Name ||
            lVar.StartOffset != rVar.StartOffset ||
            lVar.Size != rVar.Size ||
            lVar.uFlags != rVar.uFlags ||
            lVar.StartTexture != rVar.StartTexture ||
            lVar.TextureSize != rVar.TextureSize ||
            lVar.StartSampler != rVar.StartSampler ||
            lVar.SamplerSize != rVar.SamplerSize) {
            return false;
        }
        if (!IsHlslTypeEqual(l, lVar.Type, r, rVar.Type)) {
            return false;
        }
    }
    return true;
}

bool IsHlslTypeEqual(
    const HlslShaderDesc& l, size_t lType,
    const HlslShaderDesc& r, size_t rType) noexcept {
    struct TypeCompareCtx {
        size_t lTypeIdx, rTypeIdx;
    };
    stack<TypeCompareCtx> s;
    s.push({lType, rType});
    while (!s.empty()) {
        auto [lTypeIdx, rTypeIdx] = s.top();
        s.pop();
        RADRAY_ASSERT(lTypeIdx < l.Types.size());
        RADRAY_ASSERT(rTypeIdx < r.Types.size());
        const auto& lTypeDesc = l.Types[lTypeIdx];
        const auto& rTypeDesc = r.Types[rTypeIdx];
        if (lTypeDesc.Name != rTypeDesc.Name ||
            lTypeDesc.Class != rTypeDesc.Class ||
            lTypeDesc.Type != rTypeDesc.Type ||
            lTypeDesc.Rows != rTypeDesc.Rows ||
            lTypeDesc.Columns != rTypeDesc.Columns ||
            lTypeDesc.Elements != rTypeDesc.Elements ||
            lTypeDesc.Members.size() != rTypeDesc.Members.size()) {
            return false;
        }
        size_t memberCount = lTypeDesc.Members.size();
        for (size_t i = 0; i < memberCount; i++) {
            s.push({lTypeDesc.Members[i].Type, rTypeDesc.Members[i].Type});
        }
    }
    return true;
}

bool IsBufferDimension(HlslSRVDimension dim) noexcept {
    switch (dim) {
        case HlslSRVDimension::BUFFER:
        case HlslSRVDimension::BUFFEREX: return true;
        default: return false;
    }
}

std::optional<HlslShaderDesc> MergeHlslShaderDesc(std::span<const HlslShaderDesc*> descs) noexcept {
    HlslShaderDesc dstDesc{};
    auto createImcompleteType = [&](const HlslShaderDesc& srcDesc, size_t srcTypeIdx) {
        const auto& srcType = srcDesc.Types[srcTypeIdx];
        size_t dstTypeIdx = dstDesc.Types.size();
        dstDesc.Types.emplace_back(srcType);
        return dstTypeIdx;
    };
    auto createType = [&](const HlslShaderDesc& srcDesc, size_t srcTypeIdx_) {
        auto rootTypeIdx = createImcompleteType(srcDesc, srcTypeIdx_);
        struct TypeCreateCtx {
            size_t srcTypeIdx, dstTypeIdx, member;
        };
        stack<TypeCreateCtx> s;
        RADRAY_ASSERT(srcTypeIdx_ < srcDesc.Types.size());
        for (size_t i = 0; i < srcDesc.Types[srcTypeIdx_].Members.size(); i++) {
            s.push({srcTypeIdx_, rootTypeIdx, i});
        }
        while (!s.empty()) {
            auto [srcTypeIdx, dstTypeIdx, member] = s.top();
            s.pop();
            RADRAY_ASSERT(srcTypeIdx < srcDesc.Types.size());
            const auto& srcType = srcDesc.Types[srcTypeIdx];
            const auto& srcMember = srcType.Members[member];
            auto& dstType = dstDesc.Types[dstTypeIdx];
            auto& dstMember = dstType.Members[member];
            dstMember.Type = createImcompleteType(srcDesc, srcMember.Type);
            for (size_t i = 0; i < srcDesc.Types[srcMember.Type].Members.size(); i++) {
                s.push({srcMember.Type, dstMember.Type, i});
            }
        }
        return rootTypeIdx;
    };
    for (const auto descPtr : descs) {
        const auto& srcDesc = *descPtr;
        dstDesc.Stages |= srcDesc.Stages;
        for (const auto& srcRes : srcDesc.BoundResources) {
            auto it = std::find_if(dstDesc.BoundResources.begin(), dstDesc.BoundResources.end(), [&](const HlslInputBindDesc& i) {
                return i.BindPoint == srcRes.BindPoint && i.Space == srcRes.Space && i.Type == srcRes.Type;
            });
            if (it == dstDesc.BoundResources.end()) {
                auto& dstRes = dstDesc.BoundResources.emplace_back(srcRes);
                dstRes.Stages = srcDesc.Stages;
                if (srcRes.Type == HlslShaderInputType::CBUFFER) {
                    auto cbDescOpt = srcDesc.FindCBufferByName(srcRes.Name);
                    if (!cbDescOpt) {
                        RADRAY_ERR_LOG("cannot find cbuffer data during merge: {}", srcRes.Name);
                        return std::nullopt;
                    }
                    const auto& srcCb = cbDescOpt.value().get();
                    auto& dstCb = dstDesc.ConstantBuffers.emplace_back(srcCb);
                    dstCb.Variables.clear();
                    for (size_t srcVarIdx : srcCb.Variables) {
                        RADRAY_ASSERT(srcVarIdx < srcDesc.Variables.size());
                        const auto& srcVar = srcDesc.Variables[srcVarIdx];
                        auto dstVarIdx = dstDesc.Variables.size();
                        auto& dstVar = dstDesc.Variables.emplace_back(srcVar);
                        dstCb.Variables.push_back(dstVarIdx);
                        dstVar.Type = createType(srcDesc, srcVar.Type);
                    }
                }
            } else {
                if (srcRes != *it) {
                    RADRAY_ERR_LOG("resource mismatch during merge: {}", srcRes.Name);
                    return std::nullopt;
                }
                it->Stages |= srcDesc.Stages;
                if (srcRes.Type == HlslShaderInputType::CBUFFER) {
                    auto cbDescOpt = srcDesc.FindCBufferByName(srcRes.Name);
                    if (!cbDescOpt.has_value()) {
                        RADRAY_ERR_LOG("cannot find cbuffer data during merge: {}", srcRes.Name);
                        return std::nullopt;
                    }
                    auto resCbDescOpt = dstDesc.FindCBufferByName(it->Name);
                    if (!resCbDescOpt.has_value()) {
                        RADRAY_ERR_LOG("cannot find merged cbuffer data during merge: {}", srcRes.Name);
                        return std::nullopt;
                    }
                    if (!IsHlslShaderBufferEqual(srcDesc, cbDescOpt.value().get(), dstDesc, resCbDescOpt.value().get())) {
                        RADRAY_ERR_LOG("cbuffer mismatch during merge: {}", srcRes.Name);
                        return std::nullopt;
                    }
                }
            }
        }
    }
    if (descs.size() > 0) {
        dstDesc.MinFeatureLevel = descs[0]->MinFeatureLevel;
        for (size_t i = 1; i < descs.size(); i++) {
            if (dstDesc.MinFeatureLevel > descs[i]->MinFeatureLevel) {
                dstDesc.MinFeatureLevel = descs[i]->MinFeatureLevel;
            }
        }
    }
    return dstDesc;
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
#ifndef UNICODE
#define UNICODE
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
#include <dxc/dxcapi.h>
#else
#include "d3d12shader.h"
#include <dxc/dxcapi.h>
template <class T>
class ComPtr : public CComPtr<T> {
public:
    using CComPtr<T>::CComPtr;
    T* Get() const throw() { return this->operator->(); }
};
#endif

#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/platform.h>

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

class DxcImpl : public Dxc::Impl {
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

    DxcImpl(
        DynamicLibrary dxcLib,
        DynamicLibrary dxilLib,
#ifdef RADRAY_ENABLE_MIMALLOC
        ComPtr<MiMallocAdapter> mi,
#endif
        ComPtr<IDxcCompiler3> dxc,
        ComPtr<IDxcUtils> utils,
        ComPtr<IDxcIncludeHandler> inc) noexcept
        : _dxcLib(std::move(dxcLib)),
          _dxilLib(std::move(dxilLib)),
#ifdef RADRAY_ENABLE_MIMALLOC
          _mi(std::move(mi)),
#endif
          _dxc(std::move(dxc)),
          _utils(std::move(utils)),
          _inc(std::move(inc)) {
    }
    DxcImpl(const DxcImpl&) = delete;
    DxcImpl& operator=(const DxcImpl&) = delete;
    DxcImpl(DxcImpl&&) = delete;
    DxcImpl& operator=(DxcImpl&&) = delete;
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
            auto w = text_encoding::ToWideChar(i);
            if (!w.has_value()) {
                RADRAY_ERR_LOG("arg convert error: {}", i);
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
            RADRAY_ERR_LOG("IDxcCompiler3::Compile failed: {}", hr);
            return Nullable<ComPtr<IDxcResult>>{nullptr};
        }
        return Nullable<ComPtr<IDxcResult>>{std::move(compileResult)};
    }

    CompileStateData GetCompileState(IDxcResult* compileResult) noexcept {
        CompileStateData result{};
        if (HRESULT hr = compileResult->GetStatus(&result.Status);
            FAILED(hr)) {
            RADRAY_ERR_LOG("IDxcResult::GetStatus failed: {}", hr);
            result.Status = hr;
            return result;
        }
        ComPtr<IDxcBlobEncoding> errBuffer;
        if (compileResult->HasOutput(DXC_OUT_ERRORS)) {
            if (HRESULT hr = compileResult->GetErrorBuffer(&errBuffer);
                FAILED(hr)) {
                RADRAY_ERR_LOG("IDxcResult::GetErrorBuffer failed: {}", hr);
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
            RADRAY_ERR_LOG("IDxcResult::GetResult failed: {}", hr);
            return {};
        }
        auto blobStart = std::bit_cast<byte const*>(blob->GetBufferPointer());
        return {blobStart, blob->GetBufferSize()};
    }

    std::span<const byte> GetBlobData(IDxcResult* compileResult, DXC_OUT_KIND kind) noexcept {
        ComPtr<IDxcBlob> blob;
        if (HRESULT hr = compileResult->GetOutput(kind, IID_PPV_ARGS(&blob), nullptr);
            FAILED(hr)) {
            RADRAY_ERR_LOG("IDxcResult::GetOutput failed: {}", hr);
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
            compileResult = compileResultOpt.Release();
        }
        auto [status, errMsg] = GetCompileState(compileResult.Get());
        if (!errMsg.empty()) {
            RADRAY_ERR_LOG("dxc compile message");
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
            RADRAY_ERR_LOG("dxc no radray ext reflection data");
            return std::nullopt;
        }
        auto reflExtOpt = DeserializeDxcReflectionRadrayExt(radrayReflData);
        if (!reflExtOpt.has_value()) {
            RADRAY_ERR_LOG("dxc no radray ext reflection data");
            return std::nullopt;
        }
        auto argsData = ParseArgs(args);
        return DxcOutput{
            {resultData.begin(), resultData.end()},
            {reflData.begin(), reflData.end()},
            std::move(reflExtOpt.value()),
            argsData.category};
    }

    std::optional<HlslShaderDesc> GetShaderDescFromOutput(
        ShaderStage stage,
        std::span<const byte> refl,
        const DxcReflectionRadrayExt& ext) noexcept {
        if (refl.empty()) {
            return std::nullopt;
        }
        DxcBuffer buf{refl.data(), refl.size(), 0};
        ComPtr<ID3D12ShaderReflection> sr;
        if (HRESULT hr = _utils->CreateReflection(&buf, IID_PPV_ARGS(&sr));
            FAILED(hr)) {
            RADRAY_ERR_LOG("IDxcUtils::CreateReflection failed: {}", hr);
            return std::nullopt;
        }
        D3D12_SHADER_DESC shaderDesc{};
        if (HRESULT hr = sr->GetDesc(&shaderDesc);
            FAILED(hr)) {
            RADRAY_ERR_LOG("ID3D12ShaderReflection::GetDesc failed: {}", hr);
            return std::nullopt;
        }

        HlslShaderDesc result{};
        result.Creator = shaderDesc.Creator;
        result.Version = shaderDesc.Version;
        result.Flags = shaderDesc.Flags;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_9_1;
        if (SUCCEEDED(sr->GetMinFeatureLevel(&featureLevel))) {
            result.MinFeatureLevel = _MapFeatureLevel(featureLevel);
        }
        result.Stages = stage;
        sr->GetThreadGroupSize(&result.GroupSizeX, &result.GroupSizeY, &result.GroupSizeZ);

        std::vector<ID3D12ShaderReflectionType*> typeCache;
        std::function<size_t(ID3D12ShaderReflectionType*)> getTypeIndex =
            [&](ID3D12ShaderReflectionType* type) -> size_t {
            if (!type) return HlslShaderTypeId::Invalid;
            for (size_t i = 0; i < typeCache.size(); i++) {
                if (typeCache[i]->IsEqual(type) == S_OK) {
                    return i;
                }
            }
            D3D12_SHADER_TYPE_DESC typeDesc{};
            if (FAILED(type->GetDesc(&typeDesc))) return HlslShaderTypeId::Invalid;
            HlslShaderTypeDesc desc{};
            desc.Name = typeDesc.Name;
            desc.Class = _MapShaderVariableClass(typeDesc.Class);
            desc.Type = _MapShaderVariableType(typeDesc.Type);
            desc.Rows = typeDesc.Rows;
            desc.Columns = typeDesc.Columns;
            desc.Elements = typeDesc.Elements;
            desc.Offset = typeDesc.Offset;
            for (UINT i = 0; i < typeDesc.Members; i++) {
                ID3D12ShaderReflectionType* memberType = type->GetMemberTypeByIndex(i);
                LPCSTR memberName = type->GetMemberTypeName(i);
                size_t memberTypeIdx = getTypeIndex(memberType);
                auto& member = desc.Members.emplace_back();
                member.Name = memberName;
                member.Type = memberTypeIdx;
            }
            result.Types.push_back(std::move(desc));
            typeCache.push_back(type);
            return result.Types.size() - 1;
        };

        for (UINT i = 0; i < shaderDesc.ConstantBuffers; i++) {
            ID3D12ShaderReflectionConstantBuffer* cb = sr->GetConstantBufferByIndex(i);
            D3D12_SHADER_BUFFER_DESC cbDesc{};
            if (FAILED(cb->GetDesc(&cbDesc))) continue;
            auto& bufDesc = result.ConstantBuffers.emplace_back();
            bufDesc.Name = cbDesc.Name;
            bufDesc.Type = _MapCBufferType(cbDesc.Type);
            bufDesc.Size = cbDesc.Size;
            bufDesc.Flags = cbDesc.uFlags;

            for (UINT v = 0; v < cbDesc.Variables; v++) {
                ID3D12ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
                D3D12_SHADER_VARIABLE_DESC varDesc{};
                if (FAILED(var->GetDesc(&varDesc))) continue;
                auto& vDesc = result.Variables.emplace_back();
                vDesc.Name = varDesc.Name;
                vDesc.StartOffset = varDesc.StartOffset;
                vDesc.Size = varDesc.Size;
                vDesc.uFlags = varDesc.uFlags;
                vDesc.StartTexture = varDesc.StartTexture;
                vDesc.TextureSize = varDesc.TextureSize;
                vDesc.StartSampler = varDesc.StartSampler;
                vDesc.SamplerSize = varDesc.SamplerSize;
                vDesc.Type = getTypeIndex(var->GetType());
                bufDesc.Variables.push_back(result.Variables.size() - 1);
            }
        }

        for (UINT i = 0; i < shaderDesc.BoundResources; i++) {
            D3D12_SHADER_INPUT_BIND_DESC bindDesc{};
            if (FAILED(sr->GetResourceBindingDesc(i, &bindDesc))) continue;
            auto& ibDesc = result.BoundResources.emplace_back();
            ibDesc.Name = bindDesc.Name;
            ibDesc.Type = _MapInputType(bindDesc.Type);
            ibDesc.BindPoint = bindDesc.BindPoint;
            ibDesc.BindCount = bindDesc.BindCount;
            ibDesc.Flags = bindDesc.uFlags;
            ibDesc.ReturnType = _MapReturnType(bindDesc.ReturnType);
            ibDesc.Dimension = _MapSRVDimension(bindDesc.Dimension);
            ibDesc.NumSamples = bindDesc.NumSamples;
            ibDesc.Space = bindDesc.Space;
            ibDesc.Stages = stage;
        }

        for (auto& cb : result.ConstantBuffers) {
            if (cb.Type != HlslCBufferType::CBUFFER) {
                continue;
            }
            auto bindIt = std::find_if(result.BoundResources.begin(), result.BoundResources.end(), [&](const HlslInputBindDesc& bind) {
                return bind.Name == cb.Name;
            });
            if (bindIt == result.BoundResources.end()) {
                RADRAY_ERR_LOG("dxc failed to find bound resource for cbuffer: {}", cb.Name);
                return std::nullopt;
            }
            auto extIt = std::find_if(ext.CBuffers.begin(), ext.CBuffers.end(), [&](const DxcReflectionRadrayExtCBuffer& extCb) {
                return extCb.BindPoint == bindIt->BindPoint && extCb.Space == bindIt->Space;
            });
            if (extIt == ext.CBuffers.end()) {
                RADRAY_ERR_LOG("dxc no radray ext reflection data for cbuffer: {}", cb.Name);
                return std::nullopt;
            }
            cb.IsViewInHlsl = extIt->IsViewInHlsl;
        }

        for (UINT i = 0; i < shaderDesc.InputParameters; i++) {
            D3D12_SIGNATURE_PARAMETER_DESC paramDesc{};
            if (FAILED(sr->GetInputParameterDesc(i, &paramDesc))) continue;
            auto& spDesc = result.InputParameters.emplace_back();
            spDesc.SemanticName = paramDesc.SemanticName;
            spDesc.SemanticIndex = paramDesc.SemanticIndex;
            spDesc.Register = paramDesc.Register;
            spDesc.SystemValueType = _MapSystemValue(paramDesc.SystemValueType);
            spDesc.ComponentType = _MapComponentType(paramDesc.ComponentType);
            spDesc.Stream = paramDesc.Stream;
        }

        for (UINT i = 0; i < shaderDesc.OutputParameters; i++) {
            D3D12_SIGNATURE_PARAMETER_DESC paramDesc{};
            if (FAILED(sr->GetOutputParameterDesc(i, &paramDesc))) continue;
            auto& spDesc = result.OutputParameters.emplace_back();
            spDesc.SemanticName = paramDesc.SemanticName;
            spDesc.SemanticIndex = paramDesc.SemanticIndex;
            spDesc.Register = paramDesc.Register;
            spDesc.SystemValueType = _MapSystemValue(paramDesc.SystemValueType);
            spDesc.ComponentType = _MapComponentType(paramDesc.ComponentType);
            spDesc.Stream = paramDesc.Stream;
        }

        return result;
    }

public:
    DynamicLibrary _dxcLib{};
    DynamicLibrary _dxilLib{};
#ifdef RADRAY_ENABLE_MIMALLOC
    ComPtr<MiMallocAdapter> _mi;
#endif
    ComPtr<IDxcCompiler3> _dxc;
    ComPtr<IDxcUtils> _utils;
    ComPtr<IDxcIncludeHandler> _inc;
};

Nullable<shared_ptr<Dxc>> CreateDxc() noexcept {
    DynamicLibrary dxcDll{"dxcompiler"};
    if (!dxcDll.IsValid()) {
        return nullptr;
    }
    DynamicLibrary dxilDll{"dxil"};
    if (!dxilDll.IsValid()) {
        return nullptr;
    }
    auto DxcCreateInstanceF = dxcDll.GetFunction<DxcCreateInstanceProc>("DxcCreateInstance");
    if (!DxcCreateInstanceF) {
        return nullptr;
    }
    ComPtr<IDxcCompiler3> dxc;
#ifdef RADRAY_ENABLE_MIMALLOC
    auto DxcCreateInstance2F = dxcDll.GetFunction<DxcCreateInstance2Proc>("DxcCreateInstance2");
    if (!DxcCreateInstance2F) {
        return nullptr;
    }
    ComPtr<MiMallocAdapter> mi{new MiMallocAdapter{}};
    if (HRESULT hr = DxcCreateInstance2F(mi.Get(), CLSID_DxcCompiler, IID_PPV_ARGS(&dxc));
        FAILED(hr)) {
        RADRAY_ERR_LOG("DxcCreateInstance2 failed: {}", hr);
        return nullptr;
    }
#else
    if (HRESULT hr = DxcCreateInstanceF(CLSID_DxcCompiler, IID_PPV_ARGS(&dxc));
        FAILED(hr)) {
        RADRAY_ERR_LOG("DxcCreateInstance failed: {}", hr);
        return nullptr;
    }
#endif
    ComPtr<IDxcUtils> utils;
    if (HRESULT hr = DxcCreateInstanceF(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
        FAILED(hr)) {
        RADRAY_ERR_LOG("DxcCreateInstance failed: {}", hr);
        return nullptr;
    }
    ComPtr<IDxcIncludeHandler> incHandler;
    if (HRESULT hr = utils->CreateDefaultIncludeHandler(&incHandler);
        FAILED(hr)) {
        RADRAY_ERR_LOG("DxcUtils::CreateDefaultIncludeHandler failed: {}", hr);
        return nullptr;
    }
    auto implPtr = make_unique<DxcImpl>(
        std::move(dxcDll),
        std::move(dxilDll),
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

static string _FormatStageAndSm(ShaderStage stage, HlslShaderModel sm) {
    auto fmtStage = [](ShaderStage stage) -> std::string_view {
        switch (stage) {
            case ShaderStage::Vertex: return "vs";
            case ShaderStage::Pixel: return "ps";
            case ShaderStage::Compute: return "cs";
            default: return "??";
        }
    };
    auto fmtSm = [](HlslShaderModel sm) -> std::string_view {
        switch (sm) {
            case HlslShaderModel::SM60: return "6_0";
            case HlslShaderModel::SM61: return "6_1";
            case HlslShaderModel::SM62: return "6_2";
            case HlslShaderModel::SM63: return "6_3";
            case HlslShaderModel::SM64: return "6_4";
            case HlslShaderModel::SM65: return "6_5";
            case HlslShaderModel::SM66: return "6_6";
            default: return "??";
        }
    };
    return fmt::format("{}_{}", fmtStage(stage), fmtSm(sm));
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
    string smStr = _FormatStageAndSm(stage, sm);
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

std::optional<DxcOutput> Dxc::Compile(const DxcCompileParams& params) noexcept {
    string smStr = _FormatStageAndSm(params.Stage, params.SM);
    vector<std::string_view> args{};
    if (params.IsSpirv) {
        args.emplace_back("-spirv");
    }
    if (!params.EnableUnbounded) {
        args.emplace_back("-all_resources_bound");
    }
    {
        args.emplace_back("-HV");
        args.emplace_back("2021");
    }
    if (params.IsOptimize) {
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
        args.emplace_back(params.EntryPoint);
    }
    for (auto&& i : params.Includes) {
        args.emplace_back("-I");
        args.emplace_back(i);
    }
    for (auto&& i : params.Defines) {
        args.emplace_back("-D");
        args.emplace_back(i);
    }
    return static_cast<DxcImpl*>(_impl.get())->Compile(params.Code, args);
}

std::optional<HlslShaderDesc> Dxc::GetShaderDescFromOutput(
    ShaderStage stage,
    std::span<const byte> refl,
    const DxcReflectionRadrayExt& ext) noexcept {
    return static_cast<DxcImpl*>(_impl.get())->GetShaderDescFromOutput(stage, refl, ext);
}

}  // namespace radray::render

#endif
