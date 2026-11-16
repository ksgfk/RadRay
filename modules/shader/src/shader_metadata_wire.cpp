#include <radray/shader/shader_metadata_wire.h>

#include <bit>

namespace radray::shader {
namespace {
void Put32(std::span<byte> bytes, size_t offset, uint32_t value) noexcept {
    for (size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<byte>(value >> (i * 8));
}
uint32_t Get32(std::span<const byte> bytes, size_t offset) noexcept {
    uint32_t result = 0;
    for (size_t i = 0; i < 4; ++i) result |= std::to_integer<uint32_t>(bytes[offset + i]) << (i * 8);
    return result;
}
}  // namespace

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

}  // namespace radray::shader
