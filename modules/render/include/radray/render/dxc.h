#pragma once

#include <array>
#include <type_traits>

#include <radray/render/common.h>

namespace radray::render {

enum class HlslShaderModel {
    SM60,
    SM61,
    SM62,
    SM63,
    SM64,
    SM65,
    SM66
};

enum class HlslFeatureLevel {
    UNKNOWN,
    LEVEL9_1,
    LEVEL9_2,
    LEVEL9_3,
    LEVEL10_0,
    LEVEL10_1,
    LEVEL11_0,
    LEVEL11_1,
    LEVEL12_0,
    LEVEL12_1,
    LEVEL12_2
};

enum class HlslCBufferType {
    UNKNOWN,
    CBUFFER,
    TBUFFER,
    INTERFACE_POINTERS,
    RESOURCE_BIND_INFO
};

enum class HlslShaderInputType {
    UNKNOWN,
    CBUFFER,
    TBUFFER,
    TEXTURE,
    SAMPLER,
    UAV_RWTYPED,
    STRUCTURED,
    UAV_RWSTRUCTURED,
    BYTEADDRESS,
    UAV_RWBYTEADDRESS,
    UAV_APPEND_STRUCTURED,
    UAV_CONSUME_STRUCTURED,
    UAV_RWSTRUCTURED_WITH_COUNTER,
    RTACCELERATIONSTRUCTURE,
    UAV_FEEDBACKTEXTURE,
};

enum class HlslResourceReturnType {
    UNKNOWN,
    UNORM,
    SNORM,
    SINT,
    UINT,
    FLOAT,
    MIXED,
    DOUBLE,
    CONTINUED
};

enum class HlslSRVDimension {
    UNKNOWN,
    BUFFER,
    TEXTURE1D,
    TEXTURE1DARRAY,
    TEXTURE2D,
    TEXTURE2DARRAY,
    TEXTURE2DMS,
    TEXTURE2DMSARRAY,
    TEXTURE3D,
    TEXTURECUBE,
    TEXTURECUBEARRAY,
    BUFFEREX
};

enum class HlslSystemValueType {
    UNDEFINED,
    POSITION,
    CLIP_DISTANCE,
    CULL_DISTANCE,
    RENDER_TARGET_ARRAY_INDEX,
    VIEWPORT_ARRAY_INDEX,
    VERTEX_ID,
    PRIMITIVE_ID,
    INSTANCE_ID,
    IS_FRONT_FACE,
    SAMPLE_INDEX,
    FINAL_QUAD_EDGE_TESSFACTOR,
    FINAL_QUAD_INSIDE_TESSFACTOR,
    FINAL_TRI_EDGE_TESSFACTOR,
    FINAL_TRI_INSIDE_TESSFACTOR,
    FINAL_LINE_DETAIL_TESSFACTOR,
    FINAL_LINE_DENSITY_TESSFACTOR,
    BARYCENTRICS,
    SHADINGRATE,
    CULLPRIMITIVE,
    TARGET,
    DEPTH,
    COVERAGE,
    DEPTH_GREATER_EQUAL,
    DEPTH_LESS_EQUAL,
    STENCIL_REF,
    INNER_COVERAGE
};

enum class HlslRegisterComponentType {
    UNKNOWN,
    UINT32,
    SINT32,
    FLOAT32,
    UINT16,
    SINT16,
    FLOAT16,
    UINT64,
    SINT64,
    FLOAT64,
};

enum class HlslShaderVariableClass {
    UNKNOWN,
    SCALAR,
    VECTOR,
    MATRIX_ROWS,
    MATRIX_COLUMNS,
    OBJECT,
    STRUCTURE,
    ARRAY
};

enum class HlslShaderVariableType {
    UNKNOWN,
    VOID,
    BOOL,
    INT,
    FLOAT,
    STRING,
    TEXTURE,
    TEXTURE1D,
    TEXTURE2D,
    TEXTURE3D,
    TEXTURECUBE,
    SAMPLER,
    SAMPLER1D,
    SAMPLER2D,
    SAMPLER3D,
    SAMPLERCUBE,
    PIXELSHADER,
    VERTEXSHADER,
    PIXELFRAGMENT,
    VERTEXFRAGMENT,
    UINT,
    UINT8,
    GEOMETRYSHADER,
    RASTERIZER,
    DEPTHSTENCIL,
    BLEND,
    BUFFER,
    CBUFFER,
    TBUFFER,
    TEXTURE1DARRAY,
    TEXTURE2DARRAY,
    RENDERTARGETVIEW,
    DEPTHSTENCILVIEW,
    TEXTURE2DMS,
    TEXTURE2DMSARRAY,
    TEXTURECUBEARRAY,
    HULLSHADER,
    DOMAINSHADER,
    INTERFACE_POINTER,
    COMPUTESHADER,
    DOUBLE,
    RWTEXTURE1D,
    RWTEXTURE1DARRAY,
    RWTEXTURE2D,
    RWTEXTURE2DARRAY,
    RWTEXTURE3D,
    RWBUFFER,
    BYTEADDRESS_BUFFER,
    RWBYTEADDRESS_BUFFER,
    STRUCTURED_BUFFER,
    RWSTRUCTURED_BUFFER,
    APPEND_STRUCTURED_BUFFER,
    CONSUME_STRUCTURED_BUFFER,
    MIN8FLOAT,
    MIN10FLOAT,
    MIN16FLOAT,
    MIN12INT,
    MIN16INT,
    MIN16UINT,
    INT16,
    UINT16,
    FLOAT16,
    INT64,
    UINT64,
};

class DxcReflectionRadrayExtCBuffer {
public:
    string Name;
    uint32_t BindPoint;
    uint32_t Space;
    bool IsViewInHlsl{false};
};

class DxcReflectionRadrayExt {
public:
    vector<DxcReflectionRadrayExtCBuffer> CBuffers;
    uint8_t TargetType{0};  // 0: DXIL, 1: SPIR-V
};

std::optional<DxcReflectionRadrayExt> DeserializeDxcReflectionRadrayExt(std::span<const byte> data) noexcept;

class DxcOutput {
public:
    vector<byte> Data;
    vector<byte> Refl;
    DxcReflectionRadrayExt ReflExt;
    ShaderBlobCategory Category;
};

class DxcCompileParams {
public:
    std::string_view Code{};
    std::string_view EntryPoint{};
    ShaderStage Stage{};
    HlslShaderModel SM{};
    std::span<std::string_view> Defines{};
    std::span<std::string_view> Includes{};
    bool IsOptimize{};
    bool IsSpirv{};
    bool EnableUnbounded{};
};

struct HlslShaderTypeId {
    static constexpr size_t Invalid = std::numeric_limits<size_t>::max();

    size_t Value{Invalid};

    constexpr HlslShaderTypeId() noexcept = default;
    constexpr HlslShaderTypeId(size_t value) noexcept : Value(value) {}

    constexpr operator size_t() const noexcept { return Value; }

    friend auto operator<=>(const HlslShaderTypeId&, const HlslShaderTypeId&) = default;
};

class HlslShaderTypeMember;

class HlslShaderTypeDesc {
public:
    string Name;
    vector<HlslShaderTypeMember> Members;
    HlslShaderVariableClass Class{HlslShaderVariableClass::UNKNOWN};
    HlslShaderVariableType Type{HlslShaderVariableType::UNKNOWN};
    uint32_t Rows{0};
    uint32_t Columns{0};
    uint32_t Elements{0};
    uint32_t Offset{0};

    bool IsPrimitive() const noexcept;
    size_t GetSizeInBytes() const noexcept;
};

class HlslShaderTypeMember {
public:
    string Name;
    HlslShaderTypeId Type{HlslShaderTypeId::Invalid};
};

class HlslShaderVariableDesc {
public:
    string Name;
    HlslShaderTypeId Type{HlslShaderTypeId::Invalid};
    uint32_t StartOffset{0};
    uint32_t Size{0};
    uint32_t uFlags{0};
    uint32_t StartTexture{0};
    uint32_t TextureSize{0};
    uint32_t StartSampler{0};
    uint32_t SamplerSize{0};
};

class HlslShaderBufferDesc {
public:
    string Name;
    vector<size_t> Variables;
    HlslCBufferType Type{HlslCBufferType::UNKNOWN};
    uint32_t Size{0};
    uint32_t Flags{0};
    bool IsViewInHlsl{false};
};

class HlslInputBindDesc {
public:
    string Name;
    HlslShaderInputType Type{HlslShaderInputType::UNKNOWN};
    uint32_t BindPoint{0};
    uint32_t BindCount{0};
    HlslResourceReturnType ReturnType{HlslResourceReturnType::UNKNOWN};
    HlslSRVDimension Dimension{HlslSRVDimension::UNKNOWN};
    uint32_t NumSamples{0};
    uint32_t Space{0};
    uint32_t Flags{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};

    ResourceBindType MapResourceBindType() const noexcept;
    bool IsUnboundArray() const noexcept;

    friend auto operator<=>(const HlslInputBindDesc& lhs, const HlslInputBindDesc& rhs) noexcept;
    friend bool operator==(const HlslInputBindDesc& lhs, const HlslInputBindDesc& rhs) noexcept;
    friend bool operator!=(const HlslInputBindDesc& lhs, const HlslInputBindDesc& rhs) noexcept;
};

class HlslSignatureParameterDesc {
public:
    string SemanticName;
    uint32_t SemanticIndex{0};
    uint32_t Register{0};
    HlslSystemValueType SystemValueType{HlslSystemValueType::UNDEFINED};
    HlslRegisterComponentType ComponentType{HlslRegisterComponentType::UNKNOWN};
    uint32_t Stream{0};
};

class HlslShaderDesc {
public:
    vector<HlslShaderBufferDesc> ConstantBuffers;
    vector<HlslInputBindDesc> BoundResources;
    vector<HlslSignatureParameterDesc> InputParameters;
    vector<HlslSignatureParameterDesc> OutputParameters;
    vector<HlslShaderVariableDesc> Variables;
    vector<HlslShaderTypeDesc> Types;
    string Creator;
    uint32_t Version{0};
    uint32_t Flags{0};
    HlslFeatureLevel MinFeatureLevel{HlslFeatureLevel::UNKNOWN};
    uint32_t GroupSizeX{0}, GroupSizeY{0}, GroupSizeZ{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};

    std::optional<std::reference_wrapper<const HlslShaderBufferDesc>> FindCBufferByName(std::string_view name) const noexcept;
};

bool IsHlslShaderBufferEqual(const HlslShaderDesc& l, const HlslShaderBufferDesc& lcb, const HlslShaderDesc& r, const HlslShaderBufferDesc& rcb) noexcept;
bool IsHlslTypeEqual(const HlslShaderDesc& l, size_t lType, const HlslShaderDesc& r, size_t rType) noexcept;

bool IsBufferDimension(HlslSRVDimension dim) noexcept;

std::optional<HlslShaderDesc> MergeHlslShaderDesc(std::span<const HlslShaderDesc*> descs) noexcept;

}  // namespace radray::render

#ifdef RADRAY_ENABLE_DXC

#include <span>

namespace radray::render {

class Dxc : public RenderBase, public enable_shared_from_this<Dxc> {
public:
    class Impl {
    public:
        virtual ~Impl() noexcept = default;
    };

    explicit Dxc(unique_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}
    ~Dxc() noexcept override = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }
    bool IsValid() const noexcept override { return _impl != nullptr; }
    void Destroy() noexcept override;

    std::optional<DxcOutput> Compile(std::string_view code, std::span<std::string_view> args) noexcept;

    std::optional<DxcOutput> Compile(
        std::string_view code,
        std::string_view entryPoint,
        ShaderStage stage,
        HlslShaderModel sm,
        bool isOptimize,
        std::span<std::string_view> defines = {},
        std::span<std::string_view> includes = {},
        bool isSpirv = false) noexcept;

    std::optional<DxcOutput> Compile(const DxcCompileParams& params) noexcept;

    std::optional<HlslShaderDesc> GetShaderDescFromOutput(
        ShaderStage stage,
        std::span<const byte> refl,
        const DxcReflectionRadrayExt& ext) noexcept;

private:
    unique_ptr<Impl> _impl;
};

Nullable<shared_ptr<Dxc>> CreateDxc() noexcept;

}  // namespace radray::render

#endif
