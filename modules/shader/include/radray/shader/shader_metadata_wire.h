#pragma once

#include <radray/shader/shader_compiler_contract.h>

namespace radray::shader {

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

}  // namespace radray::shader
