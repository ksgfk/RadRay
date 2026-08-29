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
    // schema 6 replaced 4 and 5 atomically: an artifact from an older toolchain is rejected here
    // rather than translated, because its records cannot describe policy placement at all.
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

std::optional<CompileVariantRequest> CanonicalizeCompileVariantRequest(
    const CompileVariantRequest& request) {
    if (!IsLogicalSourceName(request.SourceName) || request.Targets == ShaderTargetMask::None ||
        (static_cast<uint8_t>(request.Targets) & ~static_cast<uint8_t>(ShaderTargetMask::All)) != 0) {
        return std::nullopt;
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
    if (HasDuplicateNames(canonical.Defines) || HasDuplicateNames(canonical.Assignments)) {
        return std::nullopt;
    }
    return canonical;
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

std::optional<vector<byte>> EncodeCanonicalCompileVariantRequest(
    const CompileVariantRequest& request) {
    const std::optional<CompileVariantRequest> canonical = CanonicalizeCompileVariantRequest(request);
    if (!canonical.has_value()) {
        return std::nullopt;
    }

    vector<byte> bytes;
    bytes.reserve(128);
    detail::AppendU32LE(bytes, kShaderWireMagic);
    detail::AppendU16LE(bytes, kShaderWireSchemaVersion);
    detail::AppendString(bytes, canonical->SourceName);
    detail::AppendU32LE(bytes, static_cast<uint32_t>(canonical->RootSource.size()));
    detail::AppendBytes(bytes, std::span<const byte>{canonical->RootSource});
    detail::AppendByte(bytes, static_cast<uint8_t>(canonical->Targets));
    detail::AppendU32LE(bytes, canonical->Policy.ShaderModel);
    detail::AppendByte(bytes, canonical->Policy.Optimize);
    detail::AppendByte(bytes, canonical->Policy.DebugInfo);
    detail::AppendByte(bytes, canonical->Policy.AllResourcesBound);
    detail::AppendByte(bytes, static_cast<uint8_t>(canonical->Policy.Warnings));
    detail::AppendU32LE(bytes, static_cast<uint32_t>(canonical->Policy.SpirvTargetEnv));
    detail::AppendU32LE(bytes, canonical->Policy.HlslVersion);
    detail::AppendU32LE(bytes, canonical->Policy.Reserved);
    detail::AppendHash(bytes, canonical->ExpectedContract);

    detail::AppendU32LE(bytes, static_cast<uint32_t>(canonical->Defines.size()));
    for (const Define& define : canonical->Defines) {
        detail::AppendString(bytes, define.Name);
        detail::AppendString(bytes, define.Value);
    }
    detail::AppendU32LE(bytes, static_cast<uint32_t>(canonical->Assignments.size()));
    for (const KeywordAssignment& assignment : canonical->Assignments) {
        detail::AppendString(bytes, assignment.Name);
        detail::AppendString(bytes, assignment.Value);
    }
    return bytes;
}

std::optional<vector<byte>> EncodeSourceContractRequest(
    const SourceContractRequest& request) {
    if (!IsLogicalSourceName(request.SourceName) || request.RootSource.empty() ||
        request.SourceName.size() > std::numeric_limits<uint32_t>::max() ||
        request.RootSource.size() > std::numeric_limits<uint32_t>::max() ||
        request.Defines.size() > std::numeric_limits<uint32_t>::max() ||
        request.Targets == ShaderTargetMask::None ||
        (static_cast<uint8_t>(request.Targets) &
         ~static_cast<uint8_t>(ShaderTargetMask::All)) != 0) {
        return std::nullopt;
    }

    SourceContractRequest canonical = request;
    std::sort(
        canonical.Defines.begin(), canonical.Defines.end(),
        [](const Define& lhs, const Define& rhs) {
            return std::tie(lhs.Name, lhs.Value) < std::tie(rhs.Name, rhs.Value);
        });
    if (HasDuplicateNames(canonical.Defines))
        return std::nullopt;

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
    return bytes;
}

}  // namespace radray::shader
