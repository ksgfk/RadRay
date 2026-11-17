#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include <radray/hash.h>
#include <radray/types.h>
#include <radray/shader/diagnostic_result.h>

namespace radray::gpu {
enum class BindingKind : uint32_t;
}  // namespace radray::gpu

namespace radray::shader {

// These values are part of the persisted compiler/render boundary. Do not reuse them for
// a different wire layout; bump the schema version instead.
inline constexpr uint32_t kShaderWireMagic = 0x59524452u;  // "RDRY" in little-endian bytes.
inline constexpr uint16_t kShaderWireSchemaVersion = 2;
inline constexpr uint32_t kShaderDiscoveryWireMagic = 0x44524452u;  // "RDRD" in little-endian bytes.
inline constexpr uint16_t kShaderDiscoveryWireSchemaVersion = 3;
inline constexpr uint32_t kShaderContractWireMagic = 0x54434452u;  // "RDCT" in little-endian bytes.
inline constexpr uint16_t kShaderContractWireSchemaVersion = 1;
inline constexpr uint16_t kShaderCompilerAbiVersion = 5;
inline constexpr uint16_t kShaderMetadataSchemaVersion = 10;
inline constexpr uint64_t kShaderToolchainIdentity = 0x0000000001090216ull;
inline constexpr uint32_t kShaderNoType = 0xffffffffu;
inline constexpr uint32_t kShaderNoSampler = 0xffffffffu;
inline constexpr uint32_t kShaderTypeFlagRowMajor = 0x1u;
inline constexpr uint32_t kShaderTypeFlagMask = kShaderTypeFlagRowMajor;

// Schema 10 sampler fields encode Vulkan enumerants. The reflector converts them to the RHI sampler state.
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

// Persisted values used by ShaderArtifactBindingRecord::Placement: where the [RootSignature] policy put
// the binding. StaticSampler owns no descriptor table slot; on Vulkan the same policy slot
// arrives as Table plus a SamplerIndex.
enum class ShaderBindingPlacement : uint32_t {
    Table = 0,
    RootDescriptor = 1,
    StaticSampler = 2,
};

// Persisted values used by ShaderArtifactTypeRecord::Kind.
enum class ShaderTypeKind : uint32_t {
    Scalar = 1,
    Vector = 2,
    Matrix = 3,
    Struct = 4,
    Array = 5,
    Member = 6,
};

// Persisted values used by ShaderArtifactTypeRecord::ScalarKind. None is for struct roots
// and struct-typed arrays whose element identity is TypeIndex.
enum class ShaderScalarKind : uint32_t {
    None = 0,
    Float = 1,
    SignedInteger = 2,
    UnsignedInteger = 3,
    Bool = 4,
};

enum class WarningPolicy : uint8_t {
    Default = 0,
    WarningsAsErrors = 1,
};

enum class SpirvTargetEnvironment : uint32_t {
    Vulkan1_2 = 0,
};

using ContractHash = Hash128;
using BytecodeHash = Hash128;
// Digest of the target-independent layout the compiler published. The resolved, target-typed
// layout is derived from it and digested separately, so this is not a program identity on its own.
using BasePipelineLayoutHash = Hash128;
using GpuArtifactHash = Hash128;

struct WireBlobRange {
    uint32_t Offset{0};
    uint32_t Size{0};

    constexpr uint32_t End() const noexcept { return Offset + Size; }

    constexpr bool IsWithin(uint32_t totalSize) const noexcept {
        return Offset <= totalSize && Size <= totalSize - Offset;
    }
};

// Metadata envelope and decoded logical records.
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
    // ShaderArtifactBindingRecord::SamplerIndex. Published on the SPIR-V lane only.
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

struct ShaderArtifactEntryRecord {
    WireBlobRange Name{};
    uint8_t Stage{0};
    uint8_t Flags{0};
    uint32_t InterfaceOffset{0};
    uint32_t InterfaceSize{0};
};

struct ShaderArtifactBindingRecord {
    WireBlobRange Name{};
    uint32_t Group{0};
    uint32_t Binding{0};
    uint32_t Type{0};  // Schema 10 binding kind code. See DecodeBindingKind and EncodeBindingKind.
    uint32_t Count{0};
    uint32_t StageMask{0};
    uint32_t Placement{static_cast<uint32_t>(ShaderBindingPlacement::Table)};
    uint32_t SamplerIndex{kShaderNoSampler};
    uint32_t Flags{0};
    // Lane-local root struct that owns this declaration's CPU payload.
    uint32_t TypeIndex{kShaderNoType};
    uint32_t BufferByteSize{0};
};

struct ShaderArtifactTypeRecord {
    WireBlobRange Name{};
    uint32_t ParentIndex{0xffffffffu};
    uint32_t Kind{0};
    uint32_t ElementCount{0};
    uint32_t Offset{0};
    uint32_t Size{0};
    uint32_t Stride{0};
    // bit 0: HLSL row_major matrix. Other bits are reserved and must be zero.
    uint32_t Flags{0};
    uint32_t TypeIndex{kShaderNoType};
    uint32_t ScalarKind{0};
    uint32_t RowCount{0};
    uint32_t ColumnCount{0};
    uint32_t ElementKind{0};
    uint32_t ElementSize{0};
    uint32_t MatrixStride{0};
};

struct ShaderArtifactRootConstantRecord {
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

// Full immutable sampler state encoded in Vulkan terms.
struct ShaderArtifactSamplerRecord {
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

    friend bool operator==(const ShaderArtifactSamplerRecord&, const ShaderArtifactSamplerRecord&) noexcept = default;
};

enum class ShaderVertexComponentType : uint32_t {
    Float = 1,
    SignedInteger = 2,
    UnsignedInteger = 3,
};

struct ShaderArtifactVertexInputRecord {
    WireBlobRange Semantic{};
    uint32_t SemanticIndex{0};
    uint32_t Location{0};
    uint32_t ComponentType{0};
    uint32_t ComponentCount{0};
    uint32_t Flags{0};
};

static_assert(sizeof(WireBlobRange) == 8);
static_assert(sizeof(WireMetadataEnvelope) == 152);
static_assert(std::is_trivially_copyable_v<WireMetadataEnvelope>);

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

using DiscoveryResult = DiagnosticResult<ShaderContract>;
using CompileVariantResult = DiagnosticResult<vector<CompileTargetLane>>;

bool IsLogicalSourceName(std::string_view sourceName) noexcept;

bool HasDuplicateNames(const vector<Define>& values) noexcept;
bool HasDuplicateNames(const vector<KeywordAssignment>& values) noexcept;

DiagnosticResult<CompileVariantRequest> CanonicalizeCompileVariantRequest(
    const CompileVariantRequest& request);

DiagnosticResult<vector<byte>> EncodeCanonicalCompileVariantRequest(
    const CompileVariantRequest& request);

DiagnosticResult<vector<byte>> EncodeSourceContractRequest(
    const SourceContractRequest& request);

DiscoveryResult DecodeWireShaderContract(std::span<const byte> blob);

// Convert between the schema 10 code in ShaderArtifactBindingRecord::Type and a GPU kind. Unknown inputs return nullopt.
std::optional<gpu::BindingKind> DecodeBindingKind(uint32_t wireCode) noexcept;
std::optional<uint32_t> EncodeBindingKind(gpu::BindingKind kind) noexcept;

// Schema 10, explicit little-endian encoding. No C++ object layout is persisted.
// Record encoders check representable ranges; artifact validation checks semantic constraints.
// Input/output spans must match the record size. Failure leaves output unchanged.
uint32_t GetShaderMetadataRecordSize(const ShaderArtifactEntryRecord& record) noexcept;
bool EncodeShaderMetadataRecord(const ShaderArtifactEntryRecord& record, std::span<byte> output) noexcept;
bool DecodeShaderMetadataRecord(std::span<const byte> input, ShaderArtifactEntryRecord& output) noexcept;

uint32_t GetShaderMetadataRecordSize(const ShaderArtifactBindingRecord& record) noexcept;
bool EncodeShaderMetadataRecord(const ShaderArtifactBindingRecord& record, std::span<byte> output) noexcept;
bool DecodeShaderMetadataRecord(std::span<const byte> input, ShaderArtifactBindingRecord& output) noexcept;

uint32_t GetShaderMetadataRecordSize(const ShaderArtifactTypeRecord& record) noexcept;
bool EncodeShaderMetadataRecord(const ShaderArtifactTypeRecord& record, std::span<byte> output) noexcept;
bool DecodeShaderMetadataRecord(std::span<const byte> input, ShaderArtifactTypeRecord& output) noexcept;

uint32_t GetShaderMetadataRecordSize(const ShaderArtifactRootConstantRecord& record) noexcept;
bool EncodeShaderMetadataRecord(const ShaderArtifactRootConstantRecord& record, std::span<byte> output) noexcept;
bool DecodeShaderMetadataRecord(std::span<const byte> input, ShaderArtifactRootConstantRecord& output) noexcept;

uint32_t GetShaderMetadataRecordSize(const ShaderArtifactVertexInputRecord& record) noexcept;
bool EncodeShaderMetadataRecord(const ShaderArtifactVertexInputRecord& record, std::span<byte> output) noexcept;
bool DecodeShaderMetadataRecord(std::span<const byte> input, ShaderArtifactVertexInputRecord& output) noexcept;

uint32_t GetShaderMetadataRecordSize(const ShaderArtifactSamplerRecord& record) noexcept;
bool EncodeShaderMetadataRecord(const ShaderArtifactSamplerRecord& record, std::span<byte> output) noexcept;
bool DecodeShaderMetadataRecord(std::span<const byte> input, ShaderArtifactSamplerRecord& output) noexcept;

bool EncodeShaderMetadataEnvelope(const WireMetadataEnvelope& envelope, std::span<byte> output) noexcept;
bool DecodeShaderMetadataEnvelope(std::span<const byte> input, WireMetadataEnvelope& output) noexcept;

bool ValidateWireMetadataEnvelope(std::span<const byte> blob, ShaderTarget expectedTarget, const GpuArtifactHash& expectedGpuArtifact) noexcept;

}  // namespace radray::shader
