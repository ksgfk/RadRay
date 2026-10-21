#include <radray/shader/shader_compiler_contract.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <tuple>

#include <fmt/format.h>

namespace radray::shader {

string Hash128::ToHex() const {
    string result;
    result.reserve(Bytes.size() * 2);
    for (const uint8_t value : Bytes) {
        fmt::format_to(std::back_inserter(result), "{:02x}", static_cast<uint32_t>(value));
    }
    return result;
}

bool ValidateWireMetadataEnvelope(
    std::span<const byte> blob,
    ShaderTarget expectedTarget,
    const GpuArtifactHash& expectedGpuArtifact) noexcept {
    if (blob.size() < sizeof(WireMetadataEnvelope)) {
        return false;
    }

    uint32_t magic = 0;
    uint16_t schema = 0;
    std::memcpy(&magic, blob.data(), sizeof(magic));
    std::memcpy(&schema, blob.data() + sizeof(magic), sizeof(schema));
    // Schema 8 replaced 4 through 7 atomically: older records cannot express
    // scalar kind, matrix shape, or non-struct array elements, so an artifact
    // from an older toolchain is rejected rather than translated.
    if (magic != kShaderWireMagic || schema != kShaderMetadataSchemaVersion) {
        return false;
    }

    WireMetadataEnvelope envelope{};
    std::memcpy(&envelope, blob.data(), sizeof(envelope));
    if (envelope.HeaderSize < sizeof(WireMetadataEnvelope) ||
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

bool HasDuplicateNames(const vector<Define>& values) noexcept {
    for (size_t index = 1; index < values.size(); ++index) {
        if (values[index - 1].Name == values[index].Name) {
            return true;
        }
    }
    return false;
}

bool HasDuplicateNames(const vector<KeywordAssignment>& values) noexcept {
    for (size_t index = 1; index < values.size(); ++index) {
        if (values[index - 1].Name == values[index].Name) {
            return true;
        }
    }
    return false;
}

DiagnosticResult<CompileVariantRequest> CanonicalizeCompileVariantRequest(
    const CompileVariantRequest& request) {
    if (!IsLogicalSourceName(request.SourceName)) {
        return {std::nullopt, "SourceName must be a relative logical source path"};
    }
    if (request.Targets == ShaderTargetMask::None ||
        (static_cast<uint8_t>(request.Targets) & ~static_cast<uint8_t>(ShaderTargetMask::All)) != 0) {
        return {std::nullopt, "Targets must select DXIL, SPIR-V, or both"};
    }
    CompileVariantRequest canonical = request;
    std::sort(
        canonical.Defines.begin(),
        canonical.Defines.end(),
        [](const Define& lhs, const Define& rhs) {
            return std::tie(lhs.Name, lhs.Value) < std::tie(rhs.Name, rhs.Value);
        });
    std::sort(
        canonical.Assignments.begin(),
        canonical.Assignments.end(),
        [](const KeywordAssignment& lhs, const KeywordAssignment& rhs) {
            return std::tie(lhs.Name, lhs.Value) < std::tie(rhs.Name, rhs.Value);
        });
    if (HasDuplicateNames(canonical.Defines)) {
        return {std::nullopt, "Defines contains duplicate names"};
    }
    if (HasDuplicateNames(canonical.Assignments)) {
        return {std::nullopt, "Assignments contains duplicate keyword group names"};
    }
    return {std::move(canonical), {}};
}

namespace detail {

void AppendByte(vector<byte>& output, uint8_t value) {
    output.push_back(static_cast<byte>(value));
}

void AppendU16LE(vector<byte>& output, uint16_t value) {
    AppendByte(output, static_cast<uint8_t>(value));
    AppendByte(output, static_cast<uint8_t>(value >> 8));
}

void AppendU32LE(vector<byte>& output, uint32_t value) {
    AppendByte(output, static_cast<uint8_t>(value));
    AppendByte(output, static_cast<uint8_t>(value >> 8));
    AppendByte(output, static_cast<uint8_t>(value >> 16));
    AppendByte(output, static_cast<uint8_t>(value >> 24));
}

void AppendString(vector<byte>& output, std::string_view value) {
    AppendU32LE(output, static_cast<uint32_t>(value.size()));
    for (const char character : value) {
        AppendByte(output, static_cast<uint8_t>(character));
    }
}

void AppendBytes(vector<byte>& output, std::span<const byte> value) {
    output.insert(output.end(), value.begin(), value.end());
}

void AppendHash(vector<byte>& output, const Hash128& value) {
    for (const uint8_t byteValue : value.Bytes) {
        AppendByte(output, byteValue);
    }
}

}  // namespace detail

DiagnosticResult<vector<byte>> EncodeCanonicalCompileVariantRequest(
    const CompileVariantRequest& request) {
    const auto canonical = CanonicalizeCompileVariantRequest(request);
    if (!canonical.Succeeded()) {
        return {std::nullopt, canonical.Diagnostics};
    }

    vector<byte> bytes;
    bytes.reserve(128);
    detail::AppendU32LE(bytes, kShaderWireMagic);
    detail::AppendU16LE(bytes, kShaderWireSchemaVersion);
    detail::AppendString(bytes, canonical.Value->SourceName);
    detail::AppendU32LE(bytes, static_cast<uint32_t>(canonical.Value->RootSource.size()));
    detail::AppendBytes(bytes, std::span<const byte>{canonical.Value->RootSource});
    detail::AppendByte(bytes, static_cast<uint8_t>(canonical.Value->Targets));
    detail::AppendU32LE(bytes, canonical.Value->Policy.ShaderModel);
    detail::AppendByte(bytes, canonical.Value->Policy.Optimize);
    detail::AppendByte(bytes, canonical.Value->Policy.DebugInfo);
    detail::AppendByte(bytes, canonical.Value->Policy.AllResourcesBound);
    detail::AppendByte(bytes, static_cast<uint8_t>(canonical.Value->Policy.Warnings));
    detail::AppendU32LE(bytes, static_cast<uint32_t>(canonical.Value->Policy.SpirvTargetEnv));
    detail::AppendU32LE(bytes, canonical.Value->Policy.HlslVersion);
    detail::AppendU32LE(bytes, canonical.Value->Policy.Reserved);
    detail::AppendHash(bytes, canonical.Value->ExpectedContract);

    detail::AppendU32LE(bytes, static_cast<uint32_t>(canonical.Value->Defines.size()));
    for (const Define& define : canonical.Value->Defines) {
        detail::AppendString(bytes, define.Name);
        detail::AppendString(bytes, define.Value);
    }
    detail::AppendU32LE(bytes, static_cast<uint32_t>(canonical.Value->Assignments.size()));
    for (const KeywordAssignment& assignment : canonical.Value->Assignments) {
        detail::AppendString(bytes, assignment.Name);
        detail::AppendString(bytes, assignment.Value);
    }
    return {std::move(bytes), {}};
}

DiagnosticResult<vector<byte>> EncodeSourceContractRequest(
    const SourceContractRequest& request) {
    if (!IsLogicalSourceName(request.SourceName)) {
        return {std::nullopt, "SourceName must be a relative logical source path"};
    }
    if (request.RootSource.empty()) {
        return {std::nullopt, "RootSource must not be empty"};
    }
    if (request.SourceName.size() > std::numeric_limits<uint32_t>::max()) {
        return {std::nullopt, "SourceName exceeds the wire length limit"};
    }
    if (request.RootSource.size() > std::numeric_limits<uint32_t>::max()) {
        return {std::nullopt, "RootSource exceeds the wire length limit"};
    }
    if (request.Defines.size() > std::numeric_limits<uint32_t>::max()) {
        return {std::nullopt, "Defines exceeds the wire count limit"};
    }
    if (request.Targets == ShaderTargetMask::None ||
        (static_cast<uint8_t>(request.Targets) & ~static_cast<uint8_t>(ShaderTargetMask::All)) != 0) {
        return {std::nullopt, "Targets must select DXIL, SPIR-V, or both"};
    }

    SourceContractRequest canonical = request;
    std::sort(
        canonical.Defines.begin(), canonical.Defines.end(),
        [](const Define& lhs, const Define& rhs) {
            return std::tie(lhs.Name, lhs.Value) < std::tie(rhs.Name, rhs.Value);
        });
    if (HasDuplicateNames(canonical.Defines))
        return {std::nullopt, "Defines contains duplicate names"};

    vector<byte> bytes;
    bytes.reserve(64 + canonical.SourceName.size() + canonical.RootSource.size());
    detail::AppendU32LE(bytes, kShaderDiscoveryWireMagic);
    detail::AppendU16LE(bytes, kShaderDiscoveryWireSchemaVersion);
    detail::AppendString(bytes, canonical.SourceName);
    detail::AppendU32LE(bytes, static_cast<uint32_t>(canonical.RootSource.size()));
    detail::AppendBytes(bytes, canonical.RootSource);
    detail::AppendByte(bytes, static_cast<uint8_t>(canonical.Targets));
    detail::AppendU32LE(bytes, canonical.Policy.ShaderModel);
    detail::AppendByte(bytes, canonical.Policy.Optimize);
    detail::AppendByte(bytes, canonical.Policy.DebugInfo);
    detail::AppendByte(bytes, canonical.Policy.AllResourcesBound);
    detail::AppendByte(bytes, static_cast<uint8_t>(canonical.Policy.Warnings));
    detail::AppendU32LE(bytes, static_cast<uint32_t>(canonical.Policy.SpirvTargetEnv));
    detail::AppendU32LE(bytes, canonical.Policy.HlslVersion);
    detail::AppendU32LE(bytes, canonical.Policy.Reserved);
    detail::AppendU32LE(bytes, static_cast<uint32_t>(canonical.Defines.size()));
    for (const Define& define : canonical.Defines) {
        detail::AppendString(bytes, define.Name);
        detail::AppendString(bytes, define.Value);
    }
    return {std::move(bytes), {}};
}

}  // namespace radray::shader
