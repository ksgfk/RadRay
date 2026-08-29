#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <radray/shader/shader_compiler_contract.h>
#include <radray/types.h>

namespace radray::shader {

enum class ShaderArtifactDecodeError : uint32_t {
    None = 0,
    TruncatedEnvelope,
    InvalidEnvelope,
    InvalidTarget,
    ToolchainMismatch,
    InvalidRecordRange,
    InvalidEntry,
    DuplicateEntry,
    InvalidBinding,
    DuplicateBinding,
    InvalidTypeRecord,
    InvalidRootConstant,
    InvalidVertexInput,
    InvalidRootSignature,
    UnsupportedSchemaVersion,
    InvalidSamplerRecord,
    InvalidPlacement,
};

struct ShaderArtifactDecodeOptions {
    ShaderTarget Target{ShaderTarget::DXIL};
    GpuArtifactHash ExpectedGpuArtifact{};
    uint64_t ExpectedToolchainIdentity{0};
};

struct ShaderArtifactBindingView {
    std::string_view Name;
    WireBindingRecord Record{};
};

class ShaderArtifactView {
public:
    ShaderArtifactView() noexcept = default;

    const WireMetadataEnvelope& Envelope() const noexcept { return _envelope; }
    std::span<const WireEntryRecord> Entries() const noexcept { return _entries; }
    std::span<const WireBindingRecord> Bindings() const noexcept { return _bindings; }
    std::span<const WireTypeRecord> Types() const noexcept { return _types; }
    std::span<const WireRootConstantRecord> RootConstants() const noexcept {
        return _rootConstants;
    }
    std::span<const WireVertexInputRecord> VertexInputs() const noexcept {
        return _vertexInputs;
    }
    std::span<const WireSamplerRecord> Samplers() const noexcept { return _samplers; }
    std::span<const byte> SerializedRootSignature() const noexcept;
    std::span<const byte> Bytecode() const noexcept;

    std::optional<std::string_view> GetName(WireBlobRange range) const noexcept;
    std::optional<std::span<const byte>> FindStageBytecode(ShaderStage stage) const noexcept;
    std::optional<ShaderArtifactBindingView> FindBinding(std::string_view name) const noexcept;

private:
    WireMetadataEnvelope _envelope{};
    vector<byte> _blob;
    vector<WireEntryRecord> _entries;
    vector<WireBindingRecord> _bindings;
    vector<WireTypeRecord> _types;
    vector<WireRootConstantRecord> _rootConstants;
    vector<WireVertexInputRecord> _vertexInputs;
    vector<WireSamplerRecord> _samplers;

    friend std::optional<ShaderArtifactView> DecodeShaderArtifact(
        std::span<const byte>,
        const ShaderArtifactDecodeOptions&,
        ShaderArtifactDecodeError*) noexcept;
};

class DxilShaderArtifactView final {
public:
    const ShaderArtifactView& Generic() const noexcept { return _view; }

private:
    explicit DxilShaderArtifactView(ShaderArtifactView view) noexcept
        : _view(std::move(view)) {}

    ShaderArtifactView _view;

    friend std::optional<DxilShaderArtifactView> DecodeDxilShaderArtifact(
        std::span<const byte>,
        const ShaderArtifactDecodeOptions&,
        ShaderArtifactDecodeError*) noexcept;
};

class SpirvShaderArtifactView final {
public:
    const ShaderArtifactView& Generic() const noexcept { return _view; }

private:
    explicit SpirvShaderArtifactView(ShaderArtifactView view) noexcept
        : _view(std::move(view)) {}

    ShaderArtifactView _view;

    friend std::optional<SpirvShaderArtifactView> DecodeSpirvShaderArtifact(
        std::span<const byte>,
        const ShaderArtifactDecodeOptions&,
        ShaderArtifactDecodeError*) noexcept;
};

std::optional<ShaderArtifactView> DecodeShaderArtifact(
    std::span<const byte> blob,
    const ShaderArtifactDecodeOptions& options,
    ShaderArtifactDecodeError* error = nullptr) noexcept;

std::optional<DxilShaderArtifactView> DecodeDxilShaderArtifact(
    std::span<const byte> blob,
    const ShaderArtifactDecodeOptions& options,
    ShaderArtifactDecodeError* error = nullptr) noexcept;

std::optional<SpirvShaderArtifactView> DecodeSpirvShaderArtifact(
    std::span<const byte> blob,
    const ShaderArtifactDecodeOptions& options,
    ShaderArtifactDecodeError* error = nullptr) noexcept;

}  // namespace radray::shader
