#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include <radray/types.h>

namespace radray::shader {

// These values are part of the persisted compiler/render boundary. Do not reuse them for
// a different wire layout; bump the schema version instead.
inline constexpr uint32_t kShaderWireMagic = 0x59524452u;  // "RDRY" in little-endian bytes.
inline constexpr uint16_t kShaderWireSchemaVersion = 2;
inline constexpr uint32_t kShaderDiscoveryWireMagic = 0x44524452u;  // "RDRD" in little-endian bytes.
inline constexpr uint16_t kShaderDiscoveryWireSchemaVersion = 3;
inline constexpr uint32_t kShaderContractWireMagic = 0x54434452u;  // "RDCT" in little-endian bytes.
inline constexpr uint16_t kShaderContractWireSchemaVersion = 1;
inline constexpr uint16_t kShaderCompilerAbiVersion = 4;
inline constexpr uint16_t kShaderMetadataSchemaVersion = 7;
inline constexpr uint32_t kShaderNoType = 0xffffffffu;
inline constexpr uint32_t kShaderNoSampler = 0xffffffffu;

enum class ShaderTarget : uint8_t {
    DXIL = 0,
    SPIRV = 1,
};

enum class ShaderTargetMask : uint8_t {
    None = 0,
    DXIL = 1u << static_cast<uint8_t>(ShaderTarget::DXIL),
    SPIRV = 1u << static_cast<uint8_t>(ShaderTarget::SPIRV),
    All = 3,
};

constexpr uint8_t ToTargetMask(ShaderTarget target) noexcept {
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(target));
}

constexpr bool HasTarget(ShaderTargetMask mask, ShaderTarget target) noexcept {
    return (static_cast<uint8_t>(mask) & ToTargetMask(target)) != 0;
}

enum class ShaderStage : uint8_t {
    Vertex = 0,
    Pixel = 1,
    Compute = 2,
};

enum class ShaderKind : uint8_t {
    Graphics = 0,
    Compute = 1,
};

// Persisted values used by WireBindingRecord::Type. These are the logical resource kinds the
// compiler's declaration table saw; each target decides on its own how a kind lands.
enum class ShaderBindingKind : uint32_t {
    CBuffer = 1,
    TypedBuffer = 2,
    RWTypedBuffer = 3,
    StructuredBuffer = 4,
    RWStructuredBuffer = 5,
    RawBuffer = 6,
    RWRawBuffer = 7,
    Texture = 8,
    RWTexture = 9,
    Sampler = 10,
};

// Persisted values used by WireBindingRecord::Placement: where the [RootSignature] policy put
// the binding. StaticSampler owns no descriptor table slot; on Vulkan the same policy slot
// arrives as Table plus a SamplerIndex.
enum class ShaderBindingPlacement : uint32_t {
    Table = 0,
    RootDescriptor = 1,
    StaticSampler = 2,
};

// Persisted values used by WireTypeRecord::Kind.
enum class ShaderTypeKind : uint32_t {
    Scalar = 1,
    Vector = 2,
    Matrix = 3,
    Struct = 4,
    Array = 5,
    Member = 6,
};

enum class WarningPolicy : uint8_t {
    Default = 0,
    WarningsAsErrors = 1,
};

enum class SpirvTargetEnvironment : uint32_t {
    Vulkan1_2 = 0,
};

struct Hash128 {
    array<uint8_t, 16> Bytes{};

    friend bool operator==(const Hash128&, const Hash128&) noexcept = default;
    friend auto operator<=>(const Hash128&, const Hash128&) noexcept = default;

    string ToHex() const;
};

using ContractHash = Hash128;
using BytecodeHash = Hash128;
// Digest of the target-independent layout the compiler published. The resolved, target-typed
// layout is derived from it and digested separately, so this is not a program identity on its own.
using BasePipelineLayoutHash = Hash128;
using GpuArtifactHash = Hash128;

static_assert(sizeof(Hash128) == 16);
static_assert(std::is_standard_layout_v<Hash128> && std::is_trivially_copyable_v<Hash128>);

struct WireBlobRange {
    uint32_t Offset{0};
    uint32_t Size{0};

    constexpr uint32_t End() const noexcept { return Offset + Size; }

    constexpr bool IsWithin(uint32_t totalSize) const noexcept {
        return Offset <= totalSize && Size <= totalSize - Offset;
    }
};

// Maps a wire binding Type code (WireBindingRecord::Type) to its binding namespace. This is
// part of the persisted wire contract so both the decoder and backend layout conversion agree.
inline constexpr uint32_t GetWireBindingNamespace(uint32_t type) noexcept {
    switch (static_cast<ShaderBindingKind>(type)) {
        case ShaderBindingKind::CBuffer:
            return 0;
        case ShaderBindingKind::TypedBuffer:
        case ShaderBindingKind::StructuredBuffer:
        case ShaderBindingKind::RawBuffer:
        case ShaderBindingKind::Texture:
            return 1;
        case ShaderBindingKind::RWTypedBuffer:
        case ShaderBindingKind::RWStructuredBuffer:
        case ShaderBindingKind::RWRawBuffer:
        case ShaderBindingKind::RWTexture:
            return 2;
        case ShaderBindingKind::Sampler:
            return 3;
    }
    return 0xffffffffu;
}

// A root descriptor is bound by GPU address, so only single-declaration buffers can take that
// placement. Textures and typed buffers need a descriptor either way.
constexpr bool CanBeRootDescriptor(ShaderBindingKind kind) noexcept {
    switch (kind) {
        case ShaderBindingKind::CBuffer:
        case ShaderBindingKind::StructuredBuffer:
        case ShaderBindingKind::RWStructuredBuffer:
        case ShaderBindingKind::RawBuffer:
        case ShaderBindingKind::RWRawBuffer:
            return true;
        default:
            return false;
    }
}

// Logical resource kind classification. These are properties of the wire kind itself, so they are
// named once here instead of being re-derived by every backend that has to pick a native descriptor
// type or a required buffer usage.
constexpr bool IsUniformBufferKind(ShaderBindingKind kind) noexcept {
    return kind == ShaderBindingKind::CBuffer;
}

constexpr bool IsTexelBufferKind(ShaderBindingKind kind) noexcept {
    return kind == ShaderBindingKind::TypedBuffer || kind == ShaderBindingKind::RWTypedBuffer;
}

constexpr bool IsImageKind(ShaderBindingKind kind) noexcept {
    return kind == ShaderBindingKind::Texture || kind == ShaderBindingKind::RWTexture;
}

constexpr bool IsWritableKind(ShaderBindingKind kind) noexcept {
    switch (kind) {
        case ShaderBindingKind::RWTypedBuffer:
        case ShaderBindingKind::RWStructuredBuffer:
        case ShaderBindingKind::RWRawBuffer:
        case ShaderBindingKind::RWTexture:
            return true;
        default:
            return false;
    }
}

// Vulkan enumerant ranges the wire is allowed to carry, plus the one flag bit and the "no clamp"
// LOD value. radrayshader cannot include volk, so the bounds are named here: the decoder validates
// against them and radrayrender static_asserts them against the real Vulkan enums, which is what
// keeps the two definitions from drifting apart.
inline constexpr uint32_t kShaderSamplerMaxFilter = 1;         // NEAREST, LINEAR
inline constexpr uint32_t kShaderSamplerMaxMipmapMode = 1;     // NEAREST, LINEAR
inline constexpr uint32_t kShaderSamplerMaxAddressMode = 4;    // REPEAT .. MIRROR_CLAMP_TO_EDGE
inline constexpr uint32_t kShaderSamplerMaxCompareOp = 7;      // NEVER .. ALWAYS
inline constexpr uint32_t kShaderSamplerMaxBorderColor = 5;    // FLOAT_TRANSPARENT_BLACK .. INT_OPAQUE_WHITE
inline constexpr uint32_t kShaderSamplerMaxReductionMode = 2;  // WEIGHTED_AVERAGE, MIN, MAX
inline constexpr uint32_t kShaderSamplerAddressModeMirrorClampToEdge = 4;
inline constexpr uint32_t kShaderSamplerReductionModeWeightedAverage = 0;
inline constexpr uint32_t kShaderSamplerFlagUnnormalizedCoordinates = 0x1u;
inline constexpr uint32_t kShaderSamplerFlagMask = kShaderSamplerFlagUnnormalizedCoordinates;
inline constexpr float kShaderSamplerLodClampNone = 1000.0f;

// Wire records 依赖定宽成员的自然布局：每个字段都是自对齐的整数类型，或由这类整数组成的
// `WireBlobRange`/`Hash128`，因此不会插入 padding，标准 ABI 布局与持久化的字节格式完全一致。
// 下方的 static_assert(size) 会把任何会改变布局的未来 ABI 变化变成编译错误，而不是静默的 wire 破坏。
struct WireMetadataEnvelope {
    uint32_t Magic{kShaderWireMagic};
    uint16_t SchemaVersion{kShaderMetadataSchemaVersion};
    uint16_t HeaderSize{152};
    uint32_t TotalSize{152};
    uint8_t Target{static_cast<uint8_t>(ShaderTarget::DXIL)};
    uint8_t StageMask{0};
    uint16_t Flags{0};
    WireBlobRange EntryRecords{};
    WireBlobRange BindingRecords{};
    WireBlobRange TypeRecords{};
    WireBlobRange RootConstantRecords{};
    WireBlobRange VertexInputRecords{};
    // Immutable sampler states in policy slot order, referenced by
    // WireBindingRecord::SamplerIndex. Published on the SPIR-V lane only.
    WireBlobRange SamplerRecords{};
    // Serialized root signature carrier. Published on the DXIL lane only, where it stays the
    // sole authority for explicit topology.
    WireBlobRange RootSignature{};
    WireBlobRange Bytecode{};
    uint64_t ToolchainIdentity{0};
    ContractHash Contract{};
    BytecodeHash BytecodeDigest{};
    BasePipelineLayoutHash BasePipelineLayoutDigest{};
    GpuArtifactHash GpuArtifact{};
};

struct WireEntryRecord {
    WireBlobRange Name{};
    uint8_t Stage{0};
    uint8_t Flags{0};
    uint16_t Reserved{0};
    uint32_t InterfaceOffset{0};
    uint32_t InterfaceSize{0};
    uint32_t Reserved2{0};
};

struct WireBindingRecord {
    WireBlobRange Name{};
    uint32_t Group{0};
    uint32_t Binding{0};
    uint32_t Type{0};
    uint32_t Count{0};
    uint32_t StageMask{0};
    uint32_t Placement{static_cast<uint32_t>(ShaderBindingPlacement::Table)};
    uint32_t SamplerIndex{kShaderNoSampler};
    uint32_t Flags{0};
    // Lane-local root struct that owns this declaration's CPU payload.
    uint32_t TypeIndex{kShaderNoType};
};

struct WireTypeRecord {
    WireBlobRange Name{};
    uint32_t ParentIndex{0xffffffffu};
    uint32_t Kind{0};
    uint32_t ElementCount{0};
    uint32_t Offset{0};
    uint32_t Size{0};
    uint32_t Stride{0};
    uint32_t Flags{0};
    uint32_t TypeIndex{kShaderNoType};
};

struct WireRootConstantRecord {
    // Declaration name of the push block. The push handle table is keyed on it.
    WireBlobRange Name{};
    uint32_t RegisterSpace{0};
    uint32_t Register{0};
    uint32_t Offset{0};
    uint32_t Size{0};
    uint32_t StageMask{0};
    uint32_t Flags{0};
    // Lane-local payload owner, or no type when policy publishes no live tree.
    uint32_t TypeIndex{kShaderNoType};
};

// A static sampler state in Vulkan terms: every field holds the official Vulkan enumerant value
// so the backend consumes it without a second mapping table. The shader module deliberately does
// not include volk; radrayrender static_asserts these against the real enums.
struct WireSamplerRecord {
    uint32_t MagFilter{0};
    uint32_t MinFilter{0};
    uint32_t MipmapMode{0};
    uint32_t AddressModeU{0};
    uint32_t AddressModeV{0};
    uint32_t AddressModeW{0};
    float MipLodBias{0.0f};
    uint32_t AnisotropyEnable{0};
    float MaxAnisotropy{1.0f};
    uint32_t CompareEnable{0};
    uint32_t CompareOp{0};
    float MinLod{0.0f};
    float MaxLod{0.0f};
    uint32_t BorderColor{0};
    uint32_t ReductionMode{0};
    // bit 0: unnormalized coordinates.
    uint32_t Flags{0};

    friend bool operator==(const WireSamplerRecord&, const WireSamplerRecord&) noexcept = default;
};

enum class ShaderVertexComponentType : uint32_t {
    Float = 1,
    SignedInteger = 2,
    UnsignedInteger = 3,
};

struct WireVertexInputRecord {
    WireBlobRange Semantic{};
    uint32_t SemanticIndex{0};
    uint32_t Location{0};
    uint32_t ComponentType{0};
    uint32_t ComponentCount{0};
    uint32_t Flags{0};
};

static_assert(sizeof(WireBlobRange) == 8);
static_assert(sizeof(WireMetadataEnvelope) == 152);
static_assert(sizeof(WireEntryRecord) == 24);
static_assert(sizeof(WireBindingRecord) == 44);
static_assert(offsetof(WireBindingRecord, TypeIndex) == 40);
static_assert(sizeof(WireTypeRecord) == 40);
static_assert(sizeof(WireRootConstantRecord) == 36);
static_assert(offsetof(WireRootConstantRecord, TypeIndex) == 32);
static_assert(sizeof(WireSamplerRecord) == 64);
static_assert(sizeof(WireVertexInputRecord) == 28);
static_assert(std::is_trivially_copyable_v<WireMetadataEnvelope>);
static_assert(std::is_trivially_copyable_v<WireEntryRecord>);
static_assert(std::is_trivially_copyable_v<WireBindingRecord>);
static_assert(std::is_trivially_copyable_v<WireTypeRecord>);
static_assert(std::is_trivially_copyable_v<WireRootConstantRecord>);
static_assert(std::is_trivially_copyable_v<WireSamplerRecord>);

bool ValidateWireMetadataEnvelope(
    std::span<const byte> blob,
    ShaderTarget expectedTarget,
    const GpuArtifactHash& expectedGpuArtifact) noexcept;

struct KeywordGroup {
    string Name;
    vector<string> Values;
};

struct EntryPoint {
    string Name;
    ShaderStage Stage{ShaderStage::Vertex};
};

struct ShaderContract {
    ShaderKind Kind{ShaderKind::Graphics};
    vector<KeywordGroup> KeywordGroups;
    vector<EntryPoint> EntryPoints;
    ContractHash Hash{};
};

struct Define {
    string Name;
    string Value;
};

struct KeywordAssignment {
    string Name;
    string Value;
};

struct CompilePolicy {
    uint32_t ShaderModel{60};
    uint8_t Optimize{1};
    uint8_t DebugInfo{0};
    uint8_t AllResourcesBound{0};
    WarningPolicy Warnings{WarningPolicy::Default};
    SpirvTargetEnvironment SpirvTargetEnv{SpirvTargetEnvironment::Vulkan1_2};
    uint32_t HlslVersion{2021};
    uint32_t Reserved{0};

    friend bool operator==(const CompilePolicy&, const CompilePolicy&) noexcept = default;
};

static_assert(sizeof(CompilePolicy) == 20);
static_assert(std::is_trivially_copyable_v<CompilePolicy>);

struct SourceContractRequest {
    string SourceName;
    vector<byte> RootSource;
    vector<Define> Defines;
    ShaderTargetMask Targets{ShaderTargetMask::None};
    CompilePolicy Policy{};
};

struct CompileVariantRequest {
    string SourceName;
    vector<byte> RootSource;
    vector<Define> Defines;
    vector<KeywordAssignment> Assignments;
    ShaderTargetMask Targets{ShaderTargetMask::None};
    CompilePolicy Policy{};
    ContractHash ExpectedContract{};
};

struct CompileDiagnostic {
    uint32_t Code{0};
    string Message;
};

struct CompileStageArtifact {
    ShaderStage Stage{ShaderStage::Vertex};
    string EntryPoint;
    vector<byte> Bytecode;
};

struct CompileTargetLane {
    ShaderTarget Target{ShaderTarget::DXIL};
    vector<CompileStageArtifact> Stages;
    vector<byte> Bytecode;
    vector<byte> Metadata;
};

enum class CompileStatus : uint8_t {
    Success = 0,
    InvalidRequest = 1,
    ContractMismatch = 2,
    TargetFailure = 3,
};

struct CompileVariantResult {
    CompileStatus Status{CompileStatus::InvalidRequest};
    vector<CompileTargetLane> Lanes;
    vector<CompileDiagnostic> Diagnostics;
};

bool IsLogicalSourceName(std::string_view sourceName) noexcept;

bool HasDuplicateNames(const vector<Define>& values) noexcept;
bool HasDuplicateNames(const vector<KeywordAssignment>& values) noexcept;

std::optional<CompileVariantRequest> CanonicalizeCompileVariantRequest(
    const CompileVariantRequest& request);

std::optional<vector<byte>> EncodeCanonicalCompileVariantRequest(
    const CompileVariantRequest& request);

std::optional<vector<byte>> EncodeSourceContractRequest(
    const SourceContractRequest& request);

}  // namespace radray::shader
