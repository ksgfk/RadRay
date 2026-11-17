#include <radray/shader/shader_compiler_contract.h>

#include <radray/binary_io.h>
#include <radray/gpu/types.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <tuple>

namespace radray::shader {

namespace {

// Schema 10 assigns consecutive wire codes starting at 1, in this order.
constexpr auto Schema10BindingKinds() noexcept {
    using enum gpu::BindingKind;
    return array{CBuffer, TypedBuffer, RWTypedBuffer, StructuredBuffer, RWStructuredBuffer,
                 RawBuffer, RWRawBuffer, Texture, RWTexture, Sampler};
}

void Put32(std::span<byte> bytes, size_t offset, uint32_t value) noexcept {
    for (size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<byte>(value >> (i * 8));
}
uint32_t Get32(std::span<const byte> bytes, size_t offset) noexcept {
    uint32_t result = 0;
    for (size_t i = 0; i < 4; ++i) result |= std::to_integer<uint32_t>(bytes[offset + i]) << (i * 8);
    return result;
}
}  // namespace

std::optional<gpu::BindingKind> DecodeBindingKind(uint32_t wireCode) noexcept {
    constexpr auto kinds = Schema10BindingKinds();
    if (wireCode == 0 || wireCode > kinds.size()) return std::nullopt;
    return kinds[wireCode - 1];
}

std::optional<uint32_t> EncodeBindingKind(gpu::BindingKind kind) noexcept {
    constexpr auto kinds = Schema10BindingKinds();
    for (uint32_t i = 0; i < kinds.size(); ++i) {
        if (kinds[i] == kind) return i + 1;
    }
    return std::nullopt;
}

uint32_t GetShaderMetadataRecordSize(const ShaderArtifactEntryRecord&) noexcept { return 20; }
bool EncodeShaderMetadataRecord(const ShaderArtifactEntryRecord& record, std::span<byte> output) noexcept {
    if (output.size() != 20) return false;
    if (record.Stage > 255u) return false;
    if (record.Flags > 0u) return false;
    Put32(output, 0, record.Name.Offset);
    Put32(output, 4, record.Name.Size);
    Put32(output, 8, (uint32_t(record.Stage) << 0));
    Put32(output, 12, record.InterfaceOffset);
    Put32(output, 16, record.InterfaceSize);
    return true;
}
bool DecodeShaderMetadataRecord(std::span<const byte> input, ShaderArtifactEntryRecord& output) noexcept {
    if (input.size() != 20) return false;
    ShaderArtifactEntryRecord record{};
    record.Name.Offset = Get32(input, 0);
    record.Name.Size = Get32(input, 4);
    const uint32_t packed2 = Get32(input, 8);
    if (packed2 & ~255u) return false;
    record.Stage = static_cast<uint8_t>((packed2 >> 0) & 255u);
    record.InterfaceOffset = Get32(input, 12);
    record.InterfaceSize = Get32(input, 16);
    output = record;
    return true;
}

uint32_t GetShaderMetadataRecordSize(const ShaderArtifactBindingRecord&) noexcept { return 36; }
bool EncodeShaderMetadataRecord(const ShaderArtifactBindingRecord& record, std::span<byte> output) noexcept {
    if (output.size() != 36) return false;
    if (record.Type > 15u) return false;
    if (record.StageMask > 7u) return false;
    if (record.Placement > 3u) return false;
    if (record.Flags > 0u) return false;
    Put32(output, 0, record.Name.Offset);
    Put32(output, 4, record.Name.Size);
    Put32(output, 8, record.Group);
    Put32(output, 12, record.Binding);
    Put32(output, 16, (uint32_t(record.Type) << 0) | (uint32_t(record.StageMask) << 4) | (uint32_t(record.Placement) << 7));
    Put32(output, 20, record.Count);
    Put32(output, 24, record.SamplerIndex);
    Put32(output, 28, record.TypeIndex);
    Put32(output, 32, record.BufferByteSize);
    return true;
}
bool DecodeShaderMetadataRecord(std::span<const byte> input, ShaderArtifactBindingRecord& output) noexcept {
    if (input.size() != 36) return false;
    ShaderArtifactBindingRecord record{};
    record.Name.Offset = Get32(input, 0);
    record.Name.Size = Get32(input, 4);
    record.Group = Get32(input, 8);
    record.Binding = Get32(input, 12);
    const uint32_t packed4 = Get32(input, 16);
    if (packed4 & ~511u) return false;
    record.Type = (packed4 >> 0) & 15u;
    record.StageMask = (packed4 >> 4) & 7u;
    record.Placement = (packed4 >> 7) & 3u;
    record.Count = Get32(input, 20);
    record.SamplerIndex = Get32(input, 24);
    record.TypeIndex = Get32(input, 28);
    record.BufferByteSize = Get32(input, 32);
    output = record;
    return true;
}

uint32_t GetShaderMetadataRecordSize(const ShaderArtifactTypeRecord&) noexcept { return 44; }
bool EncodeShaderMetadataRecord(const ShaderArtifactTypeRecord& record, std::span<byte> output) noexcept {
    if (output.size() != 44) return false;
    if (record.Kind > 7u) return false;
    if (record.ElementKind > 7u) return false;
    if (record.ScalarKind > 7u) return false;
    if (record.RowCount > 7u) return false;
    if (record.ColumnCount > 7u) return false;
    if (record.Flags > 1u) return false;
    Put32(output, 0, record.Name.Offset);
    Put32(output, 4, record.Name.Size);
    Put32(output, 8, record.ParentIndex);
    Put32(output, 12, (uint32_t(record.Kind) << 0) | (uint32_t(record.ElementKind) << 3) | (uint32_t(record.ScalarKind) << 6) | (uint32_t(record.RowCount) << 9) | (uint32_t(record.ColumnCount) << 12) | (uint32_t(record.Flags) << 15));
    Put32(output, 16, record.ElementCount);
    Put32(output, 20, record.Offset);
    Put32(output, 24, record.Size);
    Put32(output, 28, record.Stride);
    Put32(output, 32, record.TypeIndex);
    Put32(output, 36, record.ElementSize);
    Put32(output, 40, record.MatrixStride);
    return true;
}
bool DecodeShaderMetadataRecord(std::span<const byte> input, ShaderArtifactTypeRecord& output) noexcept {
    if (input.size() != 44) return false;
    ShaderArtifactTypeRecord record{};
    record.Name.Offset = Get32(input, 0);
    record.Name.Size = Get32(input, 4);
    record.ParentIndex = Get32(input, 8);
    const uint32_t packed3 = Get32(input, 12);
    if (packed3 & ~65535u) return false;
    record.Kind = (packed3 >> 0) & 7u;
    record.ElementKind = (packed3 >> 3) & 7u;
    record.ScalarKind = (packed3 >> 6) & 7u;
    record.RowCount = (packed3 >> 9) & 7u;
    record.ColumnCount = (packed3 >> 12) & 7u;
    record.Flags = (packed3 >> 15) & 1u;
    record.ElementCount = Get32(input, 16);
    record.Offset = Get32(input, 20);
    record.Size = Get32(input, 24);
    record.Stride = Get32(input, 28);
    record.TypeIndex = Get32(input, 32);
    record.ElementSize = Get32(input, 36);
    record.MatrixStride = Get32(input, 40);
    output = record;
    return true;
}

uint32_t GetShaderMetadataRecordSize(const ShaderArtifactRootConstantRecord&) noexcept { return 32; }
bool EncodeShaderMetadataRecord(const ShaderArtifactRootConstantRecord& record, std::span<byte> output) noexcept {
    if (output.size() != 32) return false;
    if (record.StageMask > 7u) return false;
    if (record.Flags > 1u) return false;
    Put32(output, 0, record.Name.Offset);
    Put32(output, 4, record.Name.Size);
    Put32(output, 8, record.RegisterSpace);
    Put32(output, 12, record.Register);
    Put32(output, 16, record.Offset);
    Put32(output, 20, record.Size);
    Put32(output, 24, (uint32_t(record.StageMask) << 0) | (record.Flags << 3));
    Put32(output, 28, record.TypeIndex);
    return true;
}
bool DecodeShaderMetadataRecord(std::span<const byte> input, ShaderArtifactRootConstantRecord& output) noexcept {
    if (input.size() != 32) return false;
    ShaderArtifactRootConstantRecord record{};
    record.Name.Offset = Get32(input, 0);
    record.Name.Size = Get32(input, 4);
    record.RegisterSpace = Get32(input, 8);
    record.Register = Get32(input, 12);
    record.Offset = Get32(input, 16);
    record.Size = Get32(input, 20);
    const uint32_t packed6 = Get32(input, 24);
    if (packed6 & ~15u) return false;
    record.StageMask = (packed6 >> 0) & 7u;
    record.Flags = (packed6 >> 3) & 1u;
    record.TypeIndex = Get32(input, 28);
    output = record;
    return true;
}

uint32_t GetShaderMetadataRecordSize(const ShaderArtifactVertexInputRecord&) noexcept { return 20; }
bool EncodeShaderMetadataRecord(const ShaderArtifactVertexInputRecord& record, std::span<byte> output) noexcept {
    if (output.size() != 20) return false;
    if (record.ComponentType > 3u) return false;
    if (record.ComponentCount > 7u) return false;
    if (record.Flags > 0u) return false;
    Put32(output, 0, record.Semantic.Offset);
    Put32(output, 4, record.Semantic.Size);
    Put32(output, 8, record.SemanticIndex);
    Put32(output, 12, record.Location);
    Put32(output, 16, (uint32_t(record.ComponentType) << 0) | (uint32_t(record.ComponentCount) << 2));
    return true;
}
bool DecodeShaderMetadataRecord(std::span<const byte> input, ShaderArtifactVertexInputRecord& output) noexcept {
    if (input.size() != 20) return false;
    ShaderArtifactVertexInputRecord record{};
    record.Semantic.Offset = Get32(input, 0);
    record.Semantic.Size = Get32(input, 4);
    record.SemanticIndex = Get32(input, 8);
    record.Location = Get32(input, 12);
    const uint32_t packed4 = Get32(input, 16);
    if (packed4 & ~31u) return false;
    record.ComponentType = (packed4 >> 0) & 3u;
    record.ComponentCount = (packed4 >> 2) & 7u;
    output = record;
    return true;
}

uint32_t GetShaderMetadataRecordSize(const ShaderArtifactSamplerRecord&) noexcept { return 20; }
bool EncodeShaderMetadataRecord(const ShaderArtifactSamplerRecord& record, std::span<byte> output) noexcept {
    if (output.size() != 20) return false;
    if (record.MagFilter > 1u) return false;
    if (record.MinFilter > 1u) return false;
    if (record.MipmapMode > 1u) return false;
    if (record.AddressModeU > 7u) return false;
    if (record.AddressModeV > 7u) return false;
    if (record.AddressModeW > 7u) return false;
    if (record.AnisotropyEnable > 1u) return false;
    if (record.CompareEnable > 1u) return false;
    if (record.CompareOp > 7u) return false;
    if (record.BorderColor > 7u) return false;
    if (record.ReductionMode > 3u) return false;
    if (record.Flags > 1u) return false;
    Put32(output, 0, (uint32_t(record.MagFilter) << 0) | (uint32_t(record.MinFilter) << 1) | (uint32_t(record.MipmapMode) << 2) | (uint32_t(record.AddressModeU) << 3) | (uint32_t(record.AddressModeV) << 6) | (uint32_t(record.AddressModeW) << 9) | (uint32_t(record.AnisotropyEnable) << 12) | (uint32_t(record.CompareEnable) << 13) | (uint32_t(record.CompareOp) << 14) | (uint32_t(record.BorderColor) << 17) | (uint32_t(record.ReductionMode) << 20) | (uint32_t(record.Flags) << 22));
    Put32(output, 4, std::bit_cast<uint32_t>(record.MipLodBias));
    Put32(output, 8, std::bit_cast<uint32_t>(record.MaxAnisotropy));
    Put32(output, 12, std::bit_cast<uint32_t>(record.MinLod));
    Put32(output, 16, std::bit_cast<uint32_t>(record.MaxLod));
    return true;
}
bool DecodeShaderMetadataRecord(std::span<const byte> input, ShaderArtifactSamplerRecord& output) noexcept {
    if (input.size() != 20) return false;
    ShaderArtifactSamplerRecord record{};
    const uint32_t packed0 = Get32(input, 0);
    if (packed0 & ~8388607u) return false;
    record.MagFilter = (packed0 >> 0) & 1u;
    record.MinFilter = (packed0 >> 1) & 1u;
    record.MipmapMode = (packed0 >> 2) & 1u;
    record.AddressModeU = (packed0 >> 3) & 7u;
    record.AddressModeV = (packed0 >> 6) & 7u;
    record.AddressModeW = (packed0 >> 9) & 7u;
    record.AnisotropyEnable = (packed0 >> 12) & 1u;
    record.CompareEnable = (packed0 >> 13) & 1u;
    record.CompareOp = (packed0 >> 14) & 7u;
    record.BorderColor = (packed0 >> 17) & 7u;
    record.ReductionMode = (packed0 >> 20) & 3u;
    record.Flags = (packed0 >> 22) & 1u;
    record.MipLodBias = std::bit_cast<float>(Get32(input, 4));
    record.MaxAnisotropy = std::bit_cast<float>(Get32(input, 8));
    record.MinLod = std::bit_cast<float>(Get32(input, 12));
    record.MaxLod = std::bit_cast<float>(Get32(input, 16));
    output = record;
    return true;
}

bool EncodeShaderMetadataEnvelope(const WireMetadataEnvelope& e, std::span<byte> output) noexcept {
    if (output.size() != 152) return false;
    Put32(output, 0, e.Magic);
    Put32(output, 4, uint32_t(e.SchemaVersion) | (uint32_t(e.HeaderSize) << 16));
    Put32(output, 8, e.TotalSize);
    Put32(output, 12, uint32_t(e.Target) | (uint32_t(e.StageMask) << 8) | (uint32_t(e.Flags) << 16));
    Put32(output, 16, e.EntryRecords.Offset);
    Put32(output, 20, e.EntryRecords.Size);
    Put32(output, 24, e.BindingRecords.Offset);
    Put32(output, 28, e.BindingRecords.Size);
    Put32(output, 32, e.TypeRecords.Offset);
    Put32(output, 36, e.TypeRecords.Size);
    Put32(output, 40, e.RootConstantRecords.Offset);
    Put32(output, 44, e.RootConstantRecords.Size);
    Put32(output, 48, e.VertexInputRecords.Offset);
    Put32(output, 52, e.VertexInputRecords.Size);
    Put32(output, 56, e.SamplerRecords.Offset);
    Put32(output, 60, e.SamplerRecords.Size);
    Put32(output, 64, e.RootSignature.Offset);
    Put32(output, 68, e.RootSignature.Size);
    Put32(output, 72, e.Bytecode.Offset);
    Put32(output, 76, e.Bytecode.Size);
    Put32(output, 80, static_cast<uint32_t>(e.ToolchainIdentity));
    Put32(output, 84, static_cast<uint32_t>(e.ToolchainIdentity >> 32));
    for (size_t j = 0; j < 16; ++j) output[88 + j] = static_cast<byte>(e.Contract.Bytes[j]);
    for (size_t j = 0; j < 16; ++j) output[104 + j] = static_cast<byte>(e.BytecodeDigest.Bytes[j]);
    for (size_t j = 0; j < 16; ++j) output[120 + j] = static_cast<byte>(e.BasePipelineLayoutDigest.Bytes[j]);
    for (size_t j = 0; j < 16; ++j) output[136 + j] = static_cast<byte>(e.GpuArtifact.Bytes[j]);
    return true;
}
bool DecodeShaderMetadataEnvelope(std::span<const byte> input, WireMetadataEnvelope& output) noexcept {
    if (input.size() != 152) return false;
    WireMetadataEnvelope e;
    e.Magic = Get32(input, 0);
    const auto version = Get32(input, 4), target = Get32(input, 12);
    e.SchemaVersion = static_cast<uint16_t>(version);
    e.HeaderSize = static_cast<uint16_t>(version >> 16);
    e.TotalSize = Get32(input, 8);
    e.Target = static_cast<uint8_t>(target);
    e.StageMask = static_cast<uint8_t>(target >> 8);
    e.Flags = static_cast<uint16_t>(target >> 16);
    e.EntryRecords = {Get32(input, 16), Get32(input, 20)};
    e.BindingRecords = {Get32(input, 24), Get32(input, 28)};
    e.TypeRecords = {Get32(input, 32), Get32(input, 36)};
    e.RootConstantRecords = {Get32(input, 40), Get32(input, 44)};
    e.VertexInputRecords = {Get32(input, 48), Get32(input, 52)};
    e.SamplerRecords = {Get32(input, 56), Get32(input, 60)};
    e.RootSignature = {Get32(input, 64), Get32(input, 68)};
    e.Bytecode = {Get32(input, 72), Get32(input, 76)};
    e.ToolchainIdentity = Get32(input, 80) | (uint64_t{Get32(input, 84)} << 32);
    for (size_t j = 0; j < 16; ++j) e.Contract.Bytes[j] = std::to_integer<uint8_t>(input[88 + j]);
    for (size_t j = 0; j < 16; ++j) e.BytecodeDigest.Bytes[j] = std::to_integer<uint8_t>(input[104 + j]);
    for (size_t j = 0; j < 16; ++j) e.BasePipelineLayoutDigest.Bytes[j] = std::to_integer<uint8_t>(input[120 + j]);
    for (size_t j = 0; j < 16; ++j) e.GpuArtifact.Bytes[j] = std::to_integer<uint8_t>(input[136 + j]);
    output = e;
    return true;
}

bool ValidateWireMetadataEnvelope(
    std::span<const byte> blob,
    ShaderTarget expectedTarget,
    const GpuArtifactHash& expectedGpuArtifact) noexcept {
    if (blob.size() < sizeof(WireMetadataEnvelope)) {
        return false;
    }

    WireMetadataEnvelope envelope{};
    DecodeShaderMetadataEnvelope(blob.first(152), envelope);
    if (envelope.Magic != kShaderWireMagic || envelope.SchemaVersion != kShaderMetadataSchemaVersion) {
        return false;
    }

    if (envelope.HeaderSize < sizeof(WireMetadataEnvelope) ||
        envelope.Flags != 0 || (envelope.StageMask & ~uint8_t{7}) != 0 || envelope.StageMask == 0 ||
        envelope.TotalSize != blob.size() ||
        envelope.Target != static_cast<uint8_t>(expectedTarget) ||
        envelope.GpuArtifact != expectedGpuArtifact ||
        envelope.HeaderSize > envelope.TotalSize || envelope.TotalSize > blob.size() ||
        (expectedTarget == ShaderTarget::SPIRV && envelope.RootSignature.Size != 0) ||
        (expectedTarget == ShaderTarget::DXIL && envelope.SamplerRecords.Size != 0)) {
        return false;
    }
    return envelope.EntryRecords.IsWithin(envelope.TotalSize) &&
           envelope.BindingRecords.IsWithin(envelope.TotalSize) &&
           envelope.TypeRecords.IsWithin(envelope.TotalSize) &&
           envelope.RootConstantRecords.IsWithin(envelope.TotalSize) &&
           envelope.VertexInputRecords.IsWithin(envelope.TotalSize) &&
           envelope.SamplerRecords.IsWithin(envelope.TotalSize) &&
           envelope.RootSignature.IsWithin(envelope.TotalSize) &&
           envelope.Bytecode.IsWithin(envelope.TotalSize) && envelope.Bytecode.Size > 0;
}

bool IsLogicalSourceName(std::string_view sourceName) noexcept {
    if (sourceName.empty() || sourceName.front() == '/' || sourceName.front() == '\\' ||
        (sourceName.size() > 1 && sourceName[1] == ':')) {
        return false;
    }
    size_t segmentStart = 0;
    while (segmentStart < sourceName.size()) {
        const size_t separator = sourceName.find('/', segmentStart);
        const size_t segmentEnd = separator == std::string_view::npos ? sourceName.size() : separator;
        const std::string_view segment = sourceName.substr(segmentStart, segmentEnd - segmentStart);
        if (segment.empty() || segment == "." || segment == ".." || segment.find('\\') != std::string_view::npos) {
            return false;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        segmentStart = separator + 1;
    }
    return true;
}

namespace {

template <typename T>
bool HasAdjacentDuplicateNames(const vector<T>& values) noexcept {
    return std::adjacent_find(values.begin(), values.end(), [](const T& a, const T& b) {
        return a.Name == b.Name;
    }) != values.end();
}

template <typename T>
void SortByNameAndValue(vector<T>& values) {
    std::sort(values.begin(), values.end(), [](const T& a, const T& b) {
        return std::tie(a.Name, a.Value) < std::tie(b.Name, b.Value);
    });
}

template <typename T>
bool FitsWireList(const vector<T>& values) noexcept {
    constexpr auto limit = std::numeric_limits<uint32_t>::max();
    return values.size() <= limit && std::all_of(values.begin(), values.end(), [](const T& item) {
        return item.Name.size() <= limit && item.Value.size() <= limit;
    });
}

template <typename Request>
std::string_view CheckRequest(const Request& request) noexcept {
    if (!IsLogicalSourceName(request.SourceName)) return "SourceName must be a relative logical source path";
    if (request.SourceName.size() > std::numeric_limits<uint32_t>::max()) return "SourceName exceeds the wire length limit";
    if (request.RootSource.size() > std::numeric_limits<uint32_t>::max()) return "RootSource exceeds the wire length limit";
    if (!FitsWireList(request.Defines)) return "Defines exceeds the wire length or count limit";
    if (request.Targets == ShaderTargetMask::None ||
        (static_cast<uint8_t>(request.Targets) & ~static_cast<uint8_t>(ShaderTargetMask::All)) != 0)
        return "Targets must select DXIL, SPIR-V, or both";
    return {};
}

template <typename Request>
void WriteRequestHeader(BinaryWriter& writer, const Request& request, uint32_t magic, uint16_t version) {
    writer.U32(magic);
    writer.U16(version);
    writer.String(request.SourceName);
    writer.Size32(request.RootSource.size());
    writer.Bytes(request.RootSource);
    writer.U8(static_cast<uint8_t>(request.Targets));
    const auto& policy = request.Policy;
    writer.U32(policy.ShaderModel);
    writer.U8(policy.Optimize);
    writer.U8(policy.DebugInfo);
    writer.U8(policy.AllResourcesBound);
    writer.U8(static_cast<uint8_t>(policy.Warnings));
    writer.U32(static_cast<uint32_t>(policy.SpirvTargetEnv));
    writer.U32(policy.HlslVersion);
    writer.U32(policy.Reserved);
}

template <typename T>
void WriteNameValueList(BinaryWriter& writer, const vector<T>& values) {
    writer.Size32(values.size());
    for (const auto& item : values) {
        writer.String(item.Name);
        writer.String(item.Value);
    }
}

// The seeds, multipliers and text terminator are part of the DXC v1 contract digest.
class ContractHash128Builder {
public:
    void AddByte(uint8_t value) noexcept {
        _low ^= value;
        _low *= 1099511628211ull;
        _high ^= static_cast<uint64_t>(value) + 0x9e3779b97f4a7c15ull;
        _high *= 14029467366897019727ull;
    }

    void AddText(std::string_view text) noexcept {
        for (const char value : text) {
            AddByte(static_cast<uint8_t>(value));
        }
        AddByte(0xff);
    }

    ContractHash Finish() const noexcept {
        return ContractHash::FromWords(_low, _high);
    }

private:
    uint64_t _low{1469598103934665603ull};
    uint64_t _high{1099511628211ull};
};

ContractHash MakeContractHash(const ShaderContract& contract) noexcept {
    ContractHash128Builder hash;
    hash.AddText(contract.Kind == ShaderKind::Graphics ? "graphics" : "compute");
    for (const KeywordGroup& group : contract.KeywordGroups) {
        hash.AddText(group.Name);
        for (const string& value : group.Values) {
            hash.AddText(value);
        }
    }
    for (const EntryPoint& entry : contract.EntryPoints) {
        hash.AddText(entry.Name);
        hash.AddByte(static_cast<uint8_t>(entry.Stage));
    }
    return hash.Finish();
}

bool ReadOwnedString(BinaryReader& reader, string& value) {
    std::string_view text;
    if (!reader.String(text)) return false;
    value.assign(text);
    return true;
}

void AddWireDiagnostic(DiscoveryResult& result, std::string_view message) {
    AppendDiagnostic(result.Diagnostics, message);
}

bool IsCanonicalContract(const ShaderContract& contract) noexcept {
    for (size_t index = 1; index < contract.KeywordGroups.size(); ++index) {
        if (contract.KeywordGroups[index - 1].Name >= contract.KeywordGroups[index].Name) {
            return false;
        }
    }
    for (const KeywordGroup& group : contract.KeywordGroups) {
        if (group.Name.empty() || group.Values.empty()) {
            return false;
        }
        for (size_t index = 1; index < group.Values.size(); ++index) {
            if (group.Values[index - 1] >= group.Values[index]) {
                return false;
            }
        }
    }
    for (size_t index = 1; index < contract.EntryPoints.size(); ++index) {
        const auto& previous = contract.EntryPoints[index - 1];
        const auto& current = contract.EntryPoints[index];
        if (std::tie(previous.Stage, previous.Name) >= std::tie(current.Stage, current.Name)) {
            return false;
        }
    }
    uint32_t vertexCount = 0;
    uint32_t pixelCount = 0;
    uint32_t computeCount = 0;
    for (const EntryPoint& entry : contract.EntryPoints) {
        if (entry.Name.empty()) {
            return false;
        }
        switch (entry.Stage) {
            case ShaderStage::Vertex:
                ++vertexCount;
                break;
            case ShaderStage::Pixel:
                ++pixelCount;
                break;
            case ShaderStage::Compute:
                ++computeCount;
                break;
            default:
                return false;
        }
    }
    return (contract.Kind == ShaderKind::Compute && computeCount == 1 &&
            vertexCount == 0 && pixelCount == 0) ||
           (contract.Kind == ShaderKind::Graphics && vertexCount == 1 &&
            pixelCount <= 1 && computeCount == 0);
}

}  // namespace

bool HasDuplicateNames(const vector<Define>& values) noexcept { return HasAdjacentDuplicateNames(values); }
bool HasDuplicateNames(const vector<KeywordAssignment>& values) noexcept { return HasAdjacentDuplicateNames(values); }

DiagnosticResult<CompileVariantRequest> CanonicalizeCompileVariantRequest(const CompileVariantRequest& request) {
    if (const auto error = CheckRequest(request); !error.empty()) return {std::nullopt, string{error}};
    if (!FitsWireList(request.Assignments)) return {std::nullopt, "Assignments exceeds the wire length or count limit"};
    CompileVariantRequest canonical = request;
    SortByNameAndValue(canonical.Defines);
    SortByNameAndValue(canonical.Assignments);
    if (HasDuplicateNames(canonical.Defines)) return {std::nullopt, "Defines contains duplicate names"};
    if (HasDuplicateNames(canonical.Assignments)) return {std::nullopt, "Assignments contains duplicate keyword group names"};
    return {std::move(canonical), {}};
}

DiagnosticResult<vector<byte>> EncodeCanonicalCompileVariantRequest(const CompileVariantRequest& request) {
    const auto canonical = CanonicalizeCompileVariantRequest(request);
    if (!canonical.Succeeded()) return {std::nullopt, canonical.Diagnostics};
    const auto& value = *canonical.Value;
    BinaryWriter writer;
    WriteRequestHeader(writer, value, kShaderWireMagic, kShaderWireSchemaVersion);
    writer.Bytes(std::as_bytes(std::span{value.ExpectedContract.Bytes}));
    WriteNameValueList(writer, value.Defines);
    WriteNameValueList(writer, value.Assignments);
    return {std::move(writer).TakeData(), {}};
}

DiagnosticResult<vector<byte>> EncodeSourceContractRequest(const SourceContractRequest& request) {
    if (const auto error = CheckRequest(request); !error.empty()) return {std::nullopt, string{error}};
    if (request.RootSource.empty()) return {std::nullopt, "RootSource must not be empty"};
    auto defines = request.Defines;
    SortByNameAndValue(defines);
    if (HasDuplicateNames(defines)) return {std::nullopt, "Defines contains duplicate names"};
    BinaryWriter writer;
    WriteRequestHeader(writer, request, kShaderDiscoveryWireMagic, kShaderDiscoveryWireSchemaVersion);
    WriteNameValueList(writer, defines);
    return {std::move(writer).TakeData(), {}};
}

DiscoveryResult DecodeWireShaderContract(std::span<const byte> blob) {
    DiscoveryResult result;
    ShaderContract contract;
    BinaryReader reader{blob};
    uint32_t magic = 0;
    uint16_t schema = 0;
    uint8_t kind = 0;
    uint8_t reserved = 0;
    uint16_t reserved2 = 0;
    uint32_t groupCount = 0;
    if (!reader.U32(magic) || !reader.U16(schema) || !reader.U8(kind) ||
        !reader.U8(reserved) || !reader.U16(reserved2) || !reader.U32(groupCount) ||
        magic != kShaderContractWireMagic ||
        schema != kShaderContractWireSchemaVersion || reserved != 0 || reserved2 != 0 ||
        groupCount > 1024) {
        AddWireDiagnostic(result, "contract blob header is invalid");
        return result;
    }
    if (kind > static_cast<uint8_t>(ShaderKind::Compute)) {
        AddWireDiagnostic(result, "contract blob kind is invalid");
        return result;
    }
    contract.Kind = static_cast<ShaderKind>(kind);
    for (uint32_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
        KeywordGroup group;
        uint32_t valueCount = 0;
        if (!ReadOwnedString(reader, group.Name) || !reader.U32(valueCount) || valueCount == 0 ||
            valueCount > 1024) {
            AddWireDiagnostic(result, "contract blob keyword group is invalid");
            return result;
        }
        group.Values.reserve(valueCount);
        for (uint32_t valueIndex = 0; valueIndex < valueCount; ++valueIndex) {
            string value;
            if (!ReadOwnedString(reader, value) || value.empty()) {
                AddWireDiagnostic(result, "contract blob keyword value is invalid");
                return result;
            }
            group.Values.push_back(std::move(value));
        }
        contract.KeywordGroups.push_back(std::move(group));
    }

    uint32_t entryCount = 0;
    if (!reader.U32(entryCount) || entryCount == 0 || entryCount > 16) {
        AddWireDiagnostic(result, "contract blob entry list is invalid");
        return result;
    }
    contract.EntryPoints.reserve(entryCount);
    for (uint32_t entryIndex = 0; entryIndex < entryCount; ++entryIndex) {
        EntryPoint entry;
        uint8_t stage = 0;
        uint8_t entryReserved = 0;
        uint16_t entryReserved2 = 0;
        if (!ReadOwnedString(reader, entry.Name) || !reader.U8(stage) || !reader.U8(entryReserved) ||
            !reader.U16(entryReserved2) || entryReserved != 0 || entryReserved2 != 0 ||
            stage > static_cast<uint8_t>(ShaderStage::Compute)) {
            AddWireDiagnostic(result, "contract blob entry is invalid");
            return result;
        }
        entry.Stage = static_cast<ShaderStage>(stage);
        contract.EntryPoints.push_back(std::move(entry));
    }

    ContractHash encodedHash{};
    std::span<const byte> hashBytes;
    if (!reader.Bytes(encodedHash.Bytes.size(), hashBytes) || !reader.AtEnd() || !IsCanonicalContract(contract)) {
        AddWireDiagnostic(result, "contract blob canonical hash or topology is invalid");
        return result;
    }
    std::memcpy(encodedHash.Bytes.data(), hashBytes.data(), hashBytes.size());
    if (MakeContractHash(contract) != encodedHash) {
        AddWireDiagnostic(result, "contract blob canonical hash or topology is invalid");
        return result;
    }
    contract.Hash = encodedHash;
    result.Value = std::move(contract);
    return result;
}

}  // namespace radray::shader
