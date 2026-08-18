#pragma once

#include <optional>
#include <span>
#include <variant>

#include <radray/render/rhi.h>
#include <radray/shader/shader_artifact.h>

namespace radray::render {

enum class BackendShaderArtifactFailure : uint32_t {
    None = 0,
    UnsupportedBackend,
    TargetMismatch,
    DecodeFailed,
    PipelineLayoutCreationFailed,
};

struct BackendShaderArtifactError {
    BackendShaderArtifactFailure Failure{BackendShaderArtifactFailure::None};
    shader::ShaderArtifactDecodeError DecodeFailure{shader::ShaderArtifactDecodeError::None};
};

class BackendShaderArtifact {
    using TypedArtifact = std::variant<shader::DxilShaderArtifactView, shader::SpirvShaderArtifactView>;

    TypedArtifact _artifact;

public:
    BackendShaderArtifact(const BackendShaderArtifact&) = delete;
    BackendShaderArtifact(BackendShaderArtifact&&) noexcept = default;
    BackendShaderArtifact& operator=(const BackendShaderArtifact&) = delete;
    BackendShaderArtifact& operator=(BackendShaderArtifact&&) noexcept = default;
    ~BackendShaderArtifact() noexcept = default;

    const shader::ShaderArtifactView& Generic() const noexcept;

    // Successful creation always supplies a layout. Callers may move it into their owner.
    unique_ptr<PipelineLayout> Layout;
    ShaderBlobCategory Category{ShaderBlobCategory::DXIL};

private:
    BackendShaderArtifact(
        shader::DxilShaderArtifactView artifact,
        unique_ptr<PipelineLayout> layout) noexcept;
    BackendShaderArtifact(
        shader::SpirvShaderArtifactView artifact,
        unique_ptr<PipelineLayout> layout) noexcept;

    friend std::optional<BackendShaderArtifact> CreateBackendShaderArtifact(
        Device&,
        std::span<const byte>,
        const shader::ShaderArtifactDecodeOptions&,
        BackendShaderArtifactError*) noexcept;
};

std::optional<shader::ShaderTarget> GetShaderTargetForBackend(
    RenderBackend backend) noexcept;

std::optional<ShaderBlobCategory> GetShaderBlobCategory(
    shader::ShaderTarget target) noexcept;

std::optional<BackendShaderArtifact> CreateBackendShaderArtifact(
    Device& device,
    std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options,
    BackendShaderArtifactError* error = nullptr) noexcept;

}  // namespace radray::render
