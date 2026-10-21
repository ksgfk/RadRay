#pragma once

#include <optional>
#include <span>
#include <variant>

#include <radray/render/pipeline_layout_types.h>
#include <radray/render/rhi.h>
#include <radray/shader/shader_artifact.h>

namespace radray::render {

struct ShaderBindingInfo {
    shader::ShaderBindingKind LogicalKind{shader::ShaderBindingKind::CBuffer};
    uint32_t Group{0};
    uint32_t Count{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    bool Dynamic{false};
    bool Immutable{false};
};

class BackendShaderArtifact;

// Owns validated CPU data. Native creation consumes it without decoding or resolving again.
class PreparedBackendShaderArtifact {
public:
    PreparedBackendShaderArtifact(const PreparedBackendShaderArtifact&) = delete;
    PreparedBackendShaderArtifact& operator=(const PreparedBackendShaderArtifact&) = delete;
    PreparedBackendShaderArtifact(PreparedBackendShaderArtifact&&) noexcept = default;
    PreparedBackendShaderArtifact& operator=(PreparedBackendShaderArtifact&&) noexcept = default;
    ~PreparedBackendShaderArtifact() noexcept = default;

    const ResolvedLayoutHash& LayoutHash() const noexcept;
    shader::ShaderTarget GetTarget() const noexcept;

private:
    PreparedBackendShaderArtifact(shader::DxilShaderArtifactView artifact, ResolvedD3D12Layout layout) noexcept;
    PreparedBackendShaderArtifact(shader::SpirvShaderArtifactView artifact, ResolvedVulkanLayout layout) noexcept;
    std::variant<shader::DxilShaderArtifactView, shader::SpirvShaderArtifactView> _artifact;
    std::variant<ResolvedD3D12Layout, ResolvedVulkanLayout> _layout;

    friend shader::DiagnosticResult<PreparedBackendShaderArtifact> PrepareBackendShaderArtifact(
        RenderBackend, std::span<const byte>, const shader::ShaderArtifactDecodeOptions&,
        const ShaderProgramLayoutRecipe&);
    friend shader::DiagnosticResult<BackendShaderArtifact> CreateBackendShaderArtifact(
        Device&, PreparedBackendShaderArtifact);
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
    // Returns the target-resolved properties of one canonical descriptor declaration. Push
    // constants and unknown names have no descriptor binding information.
    std::optional<ShaderBindingInfo> FindBindingInfo(
        std::string_view declarationName) const noexcept;

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

    friend shader::DiagnosticResult<BackendShaderArtifact> CreateBackendShaderArtifact(
        Device&, PreparedBackendShaderArtifact);

    friend shader::DiagnosticResult<BackendShaderArtifact> CreateBackendShaderArtifact(
        Device&,
        std::span<const byte>,
        const shader::ShaderArtifactDecodeOptions&,
        const ShaderProgramLayoutRecipe&);
};

std::optional<shader::ShaderTarget> GetShaderTargetForBackend(
    RenderBackend backend) noexcept;

std::optional<ShaderBlobCategory> GetShaderBlobCategory(
    shader::ShaderTarget target) noexcept;

shader::DiagnosticResult<PreparedBackendShaderArtifact> PrepareBackendShaderArtifact(
    RenderBackend backend,
    std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options,
    const ShaderProgramLayoutRecipe& recipe);

shader::DiagnosticResult<BackendShaderArtifact> CreateBackendShaderArtifact(
    Device& device,
    PreparedBackendShaderArtifact prepared);

// Standalone hash query, without native creation. A caller that may create a native artifact
// should use PrepareBackendShaderArtifact and consume its result instead of resolving twice.
shader::DiagnosticResult<ResolvedLayoutHash> ResolveBackendLayoutHash(
    RenderBackend backend,
    std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options,
    const ShaderProgramLayoutRecipe& recipe);

shader::DiagnosticResult<BackendShaderArtifact> CreateBackendShaderArtifact(
    Device& device,
    std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options,
    const ShaderProgramLayoutRecipe& recipe);

inline shader::DiagnosticResult<BackendShaderArtifact> CreateBackendShaderArtifact(
    Device& device,
    std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options) {
    return CreateBackendShaderArtifact(device, blob, options, ShaderProgramLayoutRecipe{});
}

}  // namespace radray::render
