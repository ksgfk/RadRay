#pragma once

#include <optional>

#include <radray/shader/common.h>
#include <radray/shader/hlsl.h>
#include <radray/shader/spirv.h>
#include <radray/types.h>

namespace radray::shader {

enum class ShaderScalarType : uint8_t {
    Unknown,
    Bool,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float16,
    Float32,
    Float64,
};

enum class ShaderBindingKind : uint8_t {
    Unknown,
    ConstantBuffer,
    SampledTexture,
    StorageTexture,
    Sampler,
    TypedBuffer,
    StructuredBuffer,
    RawBuffer,
    AccelerationStructure,
};

enum class ShaderResourceAccess : uint8_t {
    ReadOnly,
    WriteOnly,
    ReadWrite,
};

enum class ShaderTextureDimension : uint8_t {
    Unknown,
    Dim1D,
    Dim2D,
    Dim3D,
    Cube,
    Buffer,
};

enum class ShaderSampleType : uint8_t {
    Unknown,
    Float,
    SInt,
    UInt,
    UNorm,
    SNorm,
    Depth,
};

enum class ShaderBuiltin : uint8_t {
    None,
    Position,
    ClipDistance,
    CullDistance,
    RenderTargetArrayIndex,
    ViewportArrayIndex,
    VertexIndex,
    PrimitiveIndex,
    InstanceIndex,
    FrontFacing,
    SampleIndex,
    FragDepth,
    Coverage,
    StencilRef,
};

enum class ShaderProgramKind : uint8_t {
    Graphics,
    Compute,
};

struct ShaderBindingLocation {
    uint32_t Group{0};
    uint32_t Binding{0};

    friend bool operator==(const ShaderBindingLocation&, const ShaderBindingLocation&) = default;
};

struct ShaderValueTypeDesc {
    ShaderScalarType Scalar{ShaderScalarType::Unknown};
    uint32_t Rows{1};
    uint32_t Columns{1};
    uint32_t ArrayCount{1};
    uint32_t ArrayStride{0};
    uint32_t MatrixStride{0};
    uint32_t ByteSize{0};
    bool RowMajor{false};

    friend bool operator==(const ShaderValueTypeDesc&, const ShaderValueTypeDesc&) = default;
};

struct ShaderInterfaceFieldDesc {
    string Name;
    uint32_t Offset{0};
    uint32_t Size{0};
    ShaderValueTypeDesc Type{};
    vector<ShaderInterfaceFieldDesc> Members;

    friend bool operator==(const ShaderInterfaceFieldDesc&, const ShaderInterfaceFieldDesc&) = default;
};

struct ShaderBufferInterfaceDesc {
    uint32_t ByteSize{0};
    uint32_t ElementStride{0};
    vector<ShaderInterfaceFieldDesc> Fields;

    friend bool operator==(const ShaderBufferInterfaceDesc&, const ShaderBufferInterfaceDesc&) = default;
};

struct ShaderTextureInterfaceDesc {
    ShaderTextureDimension Dimension{ShaderTextureDimension::Unknown};
    ShaderSampleType SampleType{ShaderSampleType::Unknown};
    bool Arrayed{false};
    bool Multisampled{false};
    bool Depth{false};

    friend bool operator==(const ShaderTextureInterfaceDesc&, const ShaderTextureInterfaceDesc&) = default;
};

struct ShaderBindingDesc {
    string Name;
    uint32_t BindingIndex{0};
    ShaderBindingKind Kind{ShaderBindingKind::Unknown};
    ShaderResourceAccess Access{ShaderResourceAccess::ReadOnly};
    uint32_t Count{1};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    std::optional<ShaderBufferInterfaceDesc> Buffer{};
    std::optional<ShaderTextureInterfaceDesc> Texture{};

    bool IsUnbounded() const noexcept { return Count == 0; }

    friend bool operator==(const ShaderBindingDesc&, const ShaderBindingDesc&) = default;
};

struct ShaderBindingGroupInterfaceDesc {
    uint32_t GroupIndex{0};
    vector<ShaderBindingDesc> Bindings;

    friend bool operator==(const ShaderBindingGroupInterfaceDesc&, const ShaderBindingGroupInterfaceDesc&) = default;
};

struct ShaderStageIoDesc {
    string SemanticName;
    uint32_t SemanticIndex{0};
    uint32_t Location{0};
    ShaderBuiltin Builtin{ShaderBuiltin::None};
    ShaderValueTypeDesc Type{};

    friend bool operator==(const ShaderStageIoDesc&, const ShaderStageIoDesc&) = default;
};

struct ShaderPushConstantRangeDesc {
    string Name;
    uint32_t Offset{0};
    uint32_t Size{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    vector<ShaderInterfaceFieldDesc> Fields;

    friend bool operator==(const ShaderPushConstantRangeDesc&, const ShaderPushConstantRangeDesc&) = default;
};

struct ShaderComputeInterfaceDesc {
    uint32_t GroupSizeX{1};
    uint32_t GroupSizeY{1};
    uint32_t GroupSizeZ{1};

    friend bool operator==(const ShaderComputeInterfaceDesc&, const ShaderComputeInterfaceDesc&) = default;
};

struct ShaderStageInterfaceDesc {
    ShaderStage Stage{ShaderStage::UNKNOWN};
    vector<ShaderBindingGroupInterfaceDesc> BindingGroups;
    vector<ShaderPushConstantRangeDesc> PushConstants;
    vector<ShaderStageIoDesc> Inputs;
    vector<ShaderStageIoDesc> Outputs;
    std::optional<ShaderComputeInterfaceDesc> Compute{};

    friend bool operator==(const ShaderStageInterfaceDesc&, const ShaderStageInterfaceDesc&) = default;
};

struct ShaderInterfaceDesc {
    ShaderProgramKind Kind{ShaderProgramKind::Graphics};
    vector<ShaderBindingGroupInterfaceDesc> BindingGroups;
    vector<ShaderPushConstantRangeDesc> PushConstants;
    vector<ShaderStageIoDesc> VertexInputs;
    vector<ShaderStageIoDesc> VertexOutputs;
    vector<ShaderStageIoDesc> PixelInputs;
    vector<ShaderStageIoDesc> PixelOutputs;
    std::optional<ShaderComputeInterfaceDesc> Compute{};

    friend bool operator==(const ShaderInterfaceDesc&, const ShaderInterfaceDesc&) = default;
};

enum class ShaderDiagnosticCode : uint8_t {
    UnsupportedStage,
    InvalidReflection,
    UnsupportedType,
    InvalidBinding,
    DuplicateBinding,
    IncompatibleBinding,
    IncompatibleStageIo,
    InvalidComputeGroupSize,
    SourceUnavailable,
    CompilationFailed,
    InterfaceMismatch,
};

struct ShaderDiagnosticContext {
    std::optional<ShaderTarget> Target{};
    std::optional<uint32_t> PassIndex{};
    vector<string> VariantDefines;
    ShaderStage Stage{ShaderStage::UNKNOWN};
    std::optional<uint32_t> Group{};
    std::optional<uint32_t> Binding{};

    friend bool operator==(const ShaderDiagnosticContext&, const ShaderDiagnosticContext&) = default;
};

struct ShaderDiagnostic {
    ShaderDiagnosticCode Code{ShaderDiagnosticCode::InvalidReflection};
    string Message;
    ShaderDiagnosticContext Context{};

    friend bool operator==(const ShaderDiagnostic&, const ShaderDiagnostic&) = default;
};

struct ShaderInterfaceNormalizationOptions {
    ShaderDiagnosticContext Context{};
    vector<ShaderBindingLocation> PushConstantBindings;
};

struct ShaderStageInterfaceBuildResult {
    std::optional<ShaderStageInterfaceDesc> Interface{};
    vector<ShaderDiagnostic> Diagnostics;

    bool Succeeded() const noexcept { return Interface.has_value() && Diagnostics.empty(); }
};

struct ShaderInterfaceBuildResult {
    std::optional<ShaderInterfaceDesc> Interface{};
    vector<ShaderDiagnostic> Diagnostics;

    bool Succeeded() const noexcept { return Interface.has_value() && Diagnostics.empty(); }
};

ShaderStageInterfaceBuildResult NormalizeHlslInterface(
    const HlslShaderDesc& reflection,
    ShaderStage stage,
    const ShaderInterfaceNormalizationOptions& options = {}) noexcept;

ShaderStageInterfaceBuildResult NormalizeSpirvInterface(
    const SpirvShaderDesc& reflection,
    ShaderStage stage,
    const ShaderInterfaceNormalizationOptions& options = {}) noexcept;

ShaderInterfaceBuildResult MergeGraphicsStageInterfaces(
    const ShaderStageInterfaceDesc& vertex,
    const ShaderDiagnosticContext& context = {}) noexcept;

ShaderInterfaceBuildResult MergeGraphicsStageInterfaces(
    const ShaderStageInterfaceDesc& vertex,
    const ShaderStageInterfaceDesc& pixel,
    const ShaderDiagnosticContext& context = {}) noexcept;

ShaderInterfaceBuildResult BuildComputeShaderInterface(
    const ShaderStageInterfaceDesc& compute,
    const ShaderDiagnosticContext& context = {}) noexcept;

bool IsShaderStageInterfaceValid(const ShaderStageInterfaceDesc& interface) noexcept;
bool IsShaderInterfaceValid(const ShaderInterfaceDesc& interface) noexcept;
std::optional<vector<byte>> SerializeShaderStageInterface(
    const ShaderStageInterfaceDesc& interface) noexcept;
std::optional<vector<byte>> SerializeShaderInterface(
    const ShaderInterfaceDesc& interface) noexcept;
std::optional<ShaderStageInterfaceDesc> DeserializeShaderStageInterface(
    std::span<const byte> data) noexcept;
std::optional<ShaderInterfaceDesc> DeserializeShaderInterface(
    std::span<const byte> data) noexcept;
ShaderHash HashShaderStageInterface(const ShaderStageInterfaceDesc& interface) noexcept;
ShaderHash HashShaderInterface(const ShaderInterfaceDesc& interface) noexcept;
// Physical binding compatibility ignores reflected names and stage visibility.
// Location, kind, access, count, resource shape and constant layout must match.
bool AreShaderBindingsAbiCompatible(
    const ShaderBindingDesc& lhs,
    const ShaderBindingDesc& rhs) noexcept;
// True when projection can consume the physical resource described by complete.
// Constant-buffer projections may omit fields and use a shorter byte range.
bool IsShaderBindingAbiProjectionOf(
    const ShaderBindingDesc& projection,
    const ShaderBindingDesc& complete) noexcept;

}  // namespace radray::shader
