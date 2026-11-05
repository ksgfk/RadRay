#pragma once

#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string_view>

#include <radray/shader/shader_artifact.h>
#include <radray/shader_compiler/client.h>

namespace radray::shader_compiler::test {

inline constexpr uint32_t kVertexBit = 1u << static_cast<uint8_t>(shader::ShaderStage::Vertex);
inline constexpr uint32_t kPixelBit = 1u << static_cast<uint8_t>(shader::ShaderStage::Pixel);
inline constexpr uint32_t kComputeBit = 1u << static_cast<uint8_t>(shader::ShaderStage::Compute);

inline vector<byte> ToBytes(std::string_view text) {
    const auto* data = reinterpret_cast<const byte*>(text.data());
    return {data, data + text.size()};
}

inline std::filesystem::path ProjectPath(std::string_view relative) {
    return std::filesystem::path{RADRAY_PROJECT_DIR} / relative;
}

inline vector<byte> ReadProjectFile(std::string_view relative) {
    std::ifstream file(ProjectPath(relative), std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    vector<byte> bytes(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

inline vector<std::filesystem::path> ShaderlibIncludePaths() {
    return {ProjectPath("shaderlib")};
}

struct VariantInput {
    std::string_view SourceName;
    vector<byte> Source;
    shader::ShaderTargetMask Targets{shader::ShaderTargetMask::All};
    vector<shader::Define> Defines{};
    vector<shader::KeywordAssignment> Assignments{};
    shader::CompilePolicy Policy{};
};

// Discovers the contract and compiles the variant against it; a discovery failure is returned as
// the compile failure.
inline shader::CompileVariantResult DiscoverAndCompile(
    const Client& client,
    const VariantInput& input,
    std::span<const std::filesystem::path> includePaths) {
    const DiscoveryResult discovery = client.DiscoverSourceContract(
        shader::SourceContractRequest{
            .SourceName = string{input.SourceName},
            .RootSource = input.Source,
            .Defines = input.Defines,
            .Targets = input.Targets,
            .Policy = input.Policy},
        includePaths);
    if (!discovery.Succeeded()) {
        return {std::nullopt, discovery.Diagnostics};
    }
    return client.CompileVariant(
        shader::CompileVariantRequest{
            .SourceName = string{input.SourceName},
            .RootSource = input.Source,
            .Defines = input.Defines,
            .Assignments = input.Assignments,
            .Targets = input.Targets,
            .Policy = input.Policy,
            .ExpectedContract = discovery.Value->Hash},
        includePaths);
}

inline shader::CompileVariantResult DiscoverAndCompile(const Client& client, const VariantInput& input) {
    return DiscoverAndCompile(client, input, ShaderlibIncludePaths());
}

// Decodes one lane of a compile result with the shader module decoder, trusting the lane's own GPU
// artifact hash.
inline shader::DiagnosticResult<shader::ShaderArtifactView> DecodeLane(
    const shader::CompileVariantResult& result,
    shader::ShaderTarget target) {
    if (!result.Succeeded()) {
        return {std::nullopt, result.Diagnostics};
    }
    for (const shader::CompileTargetLane& lane : *result.Value) {
        if (lane.Target != target) {
            continue;
        }
        shader::WireMetadataEnvelope envelope{};
        if (lane.Metadata.size() >= sizeof(envelope)) {
            std::memcpy(&envelope, lane.Metadata.data(), sizeof(envelope));
        }
        return shader::DecodeShaderArtifact(
            lane.Metadata,
            {.Target = target, .ExpectedGpuArtifact = envelope.GpuArtifact, .ExpectedToolchainIdentity = shader::kShaderToolchainIdentity});
    }
    return {std::nullopt, "compile result has no lane for the requested target"};
}

inline std::string_view NameOf(const shader::ShaderArtifactView& artifact, shader::WireBlobRange range) {
    return artifact.GetName(range).value_or(std::string_view{});
}

inline std::optional<uint32_t> FindType(const shader::ShaderArtifactView& artifact, uint32_t parent, std::string_view name) {
    const auto types = artifact.Types();
    for (uint32_t index = 0; index < types.size(); ++index) {
        if (types[index].ParentIndex == parent && NameOf(artifact, types[index].Name) == name) {
            return index;
        }
    }
    return std::nullopt;
}

}  // namespace radray::shader_compiler::test
