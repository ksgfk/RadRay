#pragma once

#include <optional>
#include <span>
#include <variant>

#include <radray/render/backend/pipeline_layout_types.h>
#include <radray/render/rhi.h>
#include <radray/shader/shader_artifact.h>

namespace radray::render {

enum class BackendShaderArtifactFailure : uint32_t {
    None = 0,
    UnsupportedBackend,
    TargetMismatch,
    DecodeFailed,
    LayoutResolveFailed,
    PipelineLayoutCreationFailed,
};

struct BackendShaderArtifactError {
    BackendShaderArtifactFailure Failure{BackendShaderArtifactFailure::None};
    shader::ShaderArtifactDecodeError DecodeFailure{shader::ShaderArtifactDecodeError::None};
};

class BackendShaderArtifact {
    using TypedArtifact = std::variant<shader::DxilShaderArtifactView, shader::SpirvShaderArtifactView>;
    using TypedResolvedLayout = std::variant<ResolvedD3D12Layout, ResolvedVulkanLayout>;

    TypedArtifact _artifact;
    TypedResolvedLayout _resolvedLayout;

public:
    BackendShaderArtifact(const BackendShaderArtifact&) = delete;
    BackendShaderArtifact(BackendShaderArtifact&&) noexcept = default;
    BackendShaderArtifact& operator=(const BackendShaderArtifact&) = delete;
    BackendShaderArtifact& operator=(BackendShaderArtifact&&) noexcept = default;
    ~BackendShaderArtifact() noexcept = default;

    const shader::ShaderArtifactView& Generic() const noexcept;

    // The resolved layout the native layout was built from. It owns its data, so it stays valid
    // for as long as this artifact does.
    const ResolvedLayoutHash& LayoutHash() const noexcept;
    // True when the named declaration takes its buffer offset at bind time: a D3D12 root
    // descriptor or a Vulkan dynamic buffer descriptor. Unknown names are not dynamic.
    bool IsBindingDynamic(std::string_view declarationName) const noexcept;

    // Successful creation always supplies a layout. Callers may move it into their owner.
    unique_ptr<PipelineLayout> Layout;
    ShaderBlobCategory Category{ShaderBlobCategory::DXIL};

private:
    BackendShaderArtifact(
        shader::DxilShaderArtifactView artifact,
        ResolvedD3D12Layout resolvedLayout,
        unique_ptr<PipelineLayout> layout) noexcept;
    BackendShaderArtifact(
        shader::SpirvShaderArtifactView artifact,
        ResolvedVulkanLayout resolvedLayout,
        unique_ptr<PipelineLayout> layout) noexcept;

    friend std::optional<BackendShaderArtifact> CreateBackendShaderArtifact(
        Device&,
        std::span<const byte>,
        const shader::ShaderArtifactDecodeOptions&,
        const ShaderProgramLayoutRecipe&,
        BackendShaderArtifactError*) noexcept;
};

std::optional<shader::ShaderTarget> GetShaderTargetForBackend(
    RenderBackend backend) noexcept;

std::optional<ShaderBlobCategory> GetShaderBlobCategory(
    shader::ShaderTarget target) noexcept;

// The canonical resolved-layout hash for one backend, without creating any native object. A program
// cache keys on it, so it must be computable from the compiled artifact plus the recipe alone:
// building a pipeline layout only to read its hash would create a native layout per lookup.
std::optional<ResolvedLayoutHash> ResolveBackendLayoutHash(
    RenderBackend backend,
    std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options,
    const ShaderProgramLayoutRecipe& recipe,
    BackendShaderArtifactError* error = nullptr) noexcept;

std::optional<BackendShaderArtifact> CreateBackendShaderArtifact(
    Device& device,
    std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options,
    const ShaderProgramLayoutRecipe& recipe,
    BackendShaderArtifactError* error = nullptr) noexcept;

inline std::optional<BackendShaderArtifact> CreateBackendShaderArtifact(
    Device& device,
    std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options,
    BackendShaderArtifactError* error = nullptr) noexcept {
    return CreateBackendShaderArtifact(device, blob, options, ShaderProgramLayoutRecipe{}, error);
}

}  // namespace radray::render
