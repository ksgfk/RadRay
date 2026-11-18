#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <radray/shader/diagnostic_result.h>
#include <radray/shader/shader_compiler_contract.h>
#include <radray/types.h>

namespace radray::shader {

struct ShaderArtifactDecodeOptions {
    ShaderTarget Target{ShaderTarget::DXIL};
    GpuArtifactHash ExpectedGpuArtifact{};
    uint64_t ExpectedToolchainIdentity{0};
};

struct ShaderArtifactBindingView {
    std::string_view Name;
    ShaderArtifactBindingRecord Record{};
};

class ShaderArtifactView {
public:
    ShaderArtifactView() noexcept = default;

    const WireMetadataEnvelope& Envelope() const noexcept { return _envelope; }
    std::span<const ShaderArtifactEntryRecord> Entries() const noexcept { return _entries; }
    std::span<const ShaderArtifactBindingRecord> Bindings() const noexcept { return _bindings; }
    std::span<const ShaderArtifactTypeRecord> Types() const noexcept { return _types; }
    std::span<const ShaderArtifactRootConstantRecord> RootConstants() const noexcept {
        return _rootConstants;
    }
    std::span<const ShaderArtifactVertexInputRecord> VertexInputs() const noexcept {
        return _vertexInputs;
    }
    std::span<const ShaderArtifactSamplerRecord> Samplers() const noexcept { return _samplers; }
    std::span<const byte> SerializedRootSignature() const noexcept;
    std::span<const byte> Bytecode() const noexcept;

    std::optional<std::string_view> GetName(WireBlobRange range) const noexcept;
    std::optional<std::span<const byte>> FindStageBytecode(ShaderStage stage) const noexcept;
    std::optional<ShaderArtifactBindingView> FindBinding(std::string_view name) const noexcept;

private:
    WireMetadataEnvelope _envelope{};
    vector<byte> _blob;
    vector<ShaderArtifactEntryRecord> _entries;
    vector<ShaderArtifactBindingRecord> _bindings;
    vector<ShaderArtifactTypeRecord> _types;
    vector<ShaderArtifactRootConstantRecord> _rootConstants;
    vector<ShaderArtifactVertexInputRecord> _vertexInputs;
    vector<ShaderArtifactSamplerRecord> _samplers;

    friend DiagnosticResult<ShaderArtifactView> DecodeShaderArtifact(
        std::span<const byte>,
        const ShaderArtifactDecodeOptions&);
};

class DxilShaderArtifactView final {
public:
    const ShaderArtifactView& Generic() const noexcept { return _view; }

private:
    explicit DxilShaderArtifactView(ShaderArtifactView view) noexcept
        : _view(std::move(view)) {}

    ShaderArtifactView _view;

    friend DiagnosticResult<DxilShaderArtifactView> DecodeDxilShaderArtifact(
        std::span<const byte>,
        const ShaderArtifactDecodeOptions&);
};

class SpirvShaderArtifactView final {
public:
    const ShaderArtifactView& Generic() const noexcept { return _view; }

private:
    explicit SpirvShaderArtifactView(ShaderArtifactView view) noexcept
        : _view(std::move(view)) {}

    ShaderArtifactView _view;

    friend DiagnosticResult<SpirvShaderArtifactView> DecodeSpirvShaderArtifact(
        std::span<const byte>,
        const ShaderArtifactDecodeOptions&);
};

DiagnosticResult<ShaderArtifactView> DecodeShaderArtifact(
    std::span<const byte> blob,
    const ShaderArtifactDecodeOptions& options);

DiagnosticResult<DxilShaderArtifactView> DecodeDxilShaderArtifact(
    std::span<const byte> blob,
    const ShaderArtifactDecodeOptions& options);

DiagnosticResult<SpirvShaderArtifactView> DecodeSpirvShaderArtifact(
    std::span<const byte> blob,
    const ShaderArtifactDecodeOptions& options);

}  // namespace radray::shader
