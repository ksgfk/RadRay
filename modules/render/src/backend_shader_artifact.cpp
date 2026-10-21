#include <radray/render/backend_shader_artifact.h>

#include <utility>

namespace radray::render {
PreparedBackendShaderArtifact::PreparedBackendShaderArtifact(
    shader::DxilShaderArtifactView artifact, ResolvedD3D12Layout layout) noexcept
    : _artifact(std::move(artifact)), _layout(std::move(layout)) {}

PreparedBackendShaderArtifact::PreparedBackendShaderArtifact(
    shader::SpirvShaderArtifactView artifact, ResolvedVulkanLayout layout) noexcept
    : _artifact(std::move(artifact)), _layout(std::move(layout)) {}

const ResolvedLayoutHash& PreparedBackendShaderArtifact::LayoutHash() const noexcept {
    return std::visit([](const auto& layout) -> const ResolvedLayoutHash& { return layout.Hash; }, _layout);
}

shader::ShaderTarget PreparedBackendShaderArtifact::GetTarget() const noexcept {
    return std::holds_alternative<shader::DxilShaderArtifactView>(_artifact)
               ? shader::ShaderTarget::DXIL
               : shader::ShaderTarget::SPIRV;
}

BackendShaderArtifact::BackendShaderArtifact(
    shader::DxilShaderArtifactView artifact,
    ResolvedD3D12Layout resolvedLayout,
    unique_ptr<PipelineLayout> layout) noexcept
    : _artifact{std::move(artifact)},
      _resolvedLayout{std::move(resolvedLayout)},
      Layout{std::move(layout)},
      Category{ShaderBlobCategory::DXIL} {}

BackendShaderArtifact::BackendShaderArtifact(
    shader::SpirvShaderArtifactView artifact,
    ResolvedVulkanLayout resolvedLayout,
    unique_ptr<PipelineLayout> layout) noexcept
    : _artifact{std::move(artifact)},
      _resolvedLayout{std::move(resolvedLayout)},
      Layout{std::move(layout)},
      Category{ShaderBlobCategory::SPIRV} {}

const ResolvedLayoutHash& BackendShaderArtifact::LayoutHash() const noexcept {
    return std::visit(
        [](const auto& layout) -> const ResolvedLayoutHash& { return layout.Hash; },
        _resolvedLayout);
}

bool BackendShaderArtifact::IsBindingDynamic(std::string_view declarationName) const noexcept {
    const std::optional<ShaderBindingInfo> info = FindBindingInfo(declarationName);
    return info.has_value() && info->Dynamic;
}

std::optional<ShaderBindingInfo> BackendShaderArtifact::FindBindingInfo(
    std::string_view declarationName) const noexcept {
    if (const auto* d3d12 = std::get_if<ResolvedD3D12Layout>(&_resolvedLayout)) {
        const Nullable<const ShaderLayoutMetadataRecord*> record =
            d3d12->FindRecord(declarationName);
        if (!record.HasValue() || record->Kind != ShaderLayoutRecordKind::Descriptor) {
            return std::nullopt;
        }
        const ResolvedD3D12Binding& binding = d3d12->Bindings[record->ResolvedIndex];
        return ShaderBindingInfo{
            .LogicalKind = binding.LogicalKind,
            .Group = binding.Group,
            .Count = binding.Count,
            .Stages = binding.Stages,
            .Dynamic = binding.Placement == shader::ShaderBindingPlacement::RootDescriptor,
            .Immutable = binding.Placement == shader::ShaderBindingPlacement::StaticSampler};
    }
    const auto& vulkan = std::get<ResolvedVulkanLayout>(_resolvedLayout);
    const Nullable<const ShaderLayoutMetadataRecord*> record =
        vulkan.FindRecord(declarationName);
    if (!record.HasValue() || record->Kind != ShaderLayoutRecordKind::Descriptor) {
        return std::nullopt;
    }
    const ResolvedVulkanBinding& binding = vulkan.Bindings[record->ResolvedIndex];
    return ShaderBindingInfo{
        .LogicalKind = binding.LogicalKind,
        .Group = binding.Set,
        .Count = binding.Count,
        .Stages = binding.Stages,
        .Dynamic = binding.Placement == VulkanBufferDescriptorPlacement::Dynamic,
        .Immutable = binding.ImmutableSamplerIndex != shader::kShaderNoSampler};
}

const shader::ShaderArtifactView& BackendShaderArtifact::Generic() const noexcept {
    return std::visit(
        [](const auto& artifact) -> const shader::ShaderArtifactView& {
            return artifact.Generic();
        },
        _artifact);
}

std::optional<shader::ShaderTarget> GetShaderTargetForBackend(
    RenderBackend backend) noexcept {
    switch (backend) {
        case RenderBackend::D3D12: return shader::ShaderTarget::DXIL;
        case RenderBackend::Vulkan: return shader::ShaderTarget::SPIRV;
        case RenderBackend::MAX_COUNT: return std::nullopt;
    }
    return std::nullopt;
}

std::optional<ShaderBlobCategory> GetShaderBlobCategory(
    shader::ShaderTarget target) noexcept {
    switch (target) {
        case shader::ShaderTarget::DXIL: return ShaderBlobCategory::DXIL;
        case shader::ShaderTarget::SPIRV: return ShaderBlobCategory::SPIRV;
    }
    return std::nullopt;
}

shader::DiagnosticResult<PreparedBackendShaderArtifact> PrepareBackendShaderArtifact(
    RenderBackend backend,
    std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options,
    const ShaderProgramLayoutRecipe& recipe) {
    const std::optional<shader::ShaderTarget> backendTarget = GetShaderTargetForBackend(backend);
    if (!backendTarget.has_value()) {
        return {std::nullopt, "shader artifact backend is unsupported or was not built"};
    }
    if (backendTarget.value() != options.Target) {
        return {std::nullopt, "shader artifact target does not match the requested device/backend"};
    }
    switch (options.Target) {
        case shader::ShaderTarget::DXIL: {
            auto artifact =
                shader::DecodeDxilShaderArtifact(blob, options);
            if (!artifact.Succeeded()) {
                return {std::nullopt, std::move(artifact.Diagnostics)};
            }
            std::optional<ResolvedD3D12Layout> resolved =
                ResolveD3D12Layout(*artifact.Value, recipe.D3D12);
            if (!resolved.has_value()) {
                return {std::nullopt, "D3D12 shader artifact pipeline layout resolution failed"};
            }
            return {PreparedBackendShaderArtifact{std::move(*artifact.Value), std::move(*resolved)}, std::move(artifact.Diagnostics)};
        }
        case shader::ShaderTarget::SPIRV: {
            auto artifact =
                shader::DecodeSpirvShaderArtifact(blob, options);
            if (!artifact.Succeeded()) {
                return {std::nullopt, std::move(artifact.Diagnostics)};
            }
            std::optional<ResolvedVulkanLayout> resolved =
                ResolveVulkanLayout(*artifact.Value, recipe.Vulkan);
            if (!resolved.has_value()) {
                return {std::nullopt, "Vulkan shader artifact pipeline layout resolution failed"};
            }
            return {PreparedBackendShaderArtifact{std::move(*artifact.Value), std::move(*resolved)}, std::move(artifact.Diagnostics)};
        }
    }
    return {std::nullopt, "shader artifact target does not match the requested device/backend"};
}

shader::DiagnosticResult<ResolvedLayoutHash> ResolveBackendLayoutHash(
    RenderBackend backend, std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options, const ShaderProgramLayoutRecipe& recipe) {
    auto prepared = PrepareBackendShaderArtifact(backend, blob, options, recipe);
    if (!prepared.Succeeded()) return {std::nullopt, std::move(prepared.Diagnostics)};
    return {prepared.Value->LayoutHash(), std::move(prepared.Diagnostics)};
}

shader::DiagnosticResult<BackendShaderArtifact> CreateBackendShaderArtifact(
    Device& device, std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options, const ShaderProgramLayoutRecipe& recipe) {
    auto prepared = PrepareBackendShaderArtifact(device.GetBackend(), blob, options, recipe);
    if (!prepared.Succeeded()) return {std::nullopt, std::move(prepared.Diagnostics)};
    auto created = CreateBackendShaderArtifact(device, std::move(*prepared.Value));
    shader::AppendDiagnostic(prepared.Diagnostics, created.Diagnostics);
    created.Diagnostics = std::move(prepared.Diagnostics);
    return created;
}

shader::DiagnosticResult<BackendShaderArtifact> CreateBackendShaderArtifact(
    Device& device,
    PreparedBackendShaderArtifact prepared) {
    const std::optional<shader::ShaderTarget> backendTarget =
        GetShaderTargetForBackend(device.GetBackend());
    if (!backendTarget.has_value()) {
        return {std::nullopt, "shader artifact backend is unsupported or was not built"};
    }
    if (backendTarget.value() != prepared.GetTarget()) {
        return {std::nullopt, "shader artifact target does not match the requested device/backend"};
    }

    switch (prepared.GetTarget()) {
        case shader::ShaderTarget::DXIL: {
#if defined(RADRAY_ENABLE_D3D12)
            auto& artifact = std::get<shader::DxilShaderArtifactView>(prepared._artifact);
            auto& resolved = std::get<ResolvedD3D12Layout>(prepared._layout);
            auto* factory = dynamic_cast<detail::D3D12PipelineLayoutFactory*>(&device);
            Nullable<unique_ptr<PipelineLayout>> layout = factory ? factory->CreatePipelineLayout(resolved) : nullptr;
            if (!layout.HasValue()) {
                return {std::nullopt, "D3D12 shader artifact native pipeline layout creation failed"};
            }
            return {BackendShaderArtifact{
                        std::move(artifact),
                        std::move(resolved),
                        layout.Release()},
                    {}};
#else
            return {std::nullopt, "shader artifact backend is unsupported or was not built"};
#endif
        }
        case shader::ShaderTarget::SPIRV: {
#if defined(RADRAY_ENABLE_VULKAN)
            auto& artifact = std::get<shader::SpirvShaderArtifactView>(prepared._artifact);
            auto& resolved = std::get<ResolvedVulkanLayout>(prepared._layout);
            auto* factory = dynamic_cast<detail::VulkanPipelineLayoutFactory*>(&device);
            Nullable<unique_ptr<PipelineLayout>> layout = factory ? factory->CreatePipelineLayout(resolved) : nullptr;
            if (!layout.HasValue()) {
                return {std::nullopt, "Vulkan shader artifact native pipeline layout creation failed"};
            }
            return {BackendShaderArtifact{
                        std::move(artifact),
                        std::move(resolved),
                        layout.Release()},
                    {}};
#else
            return {std::nullopt, "shader artifact backend is unsupported or was not built"};
#endif
        }
    }
    return {std::nullopt, "shader artifact target does not match the requested device/backend"};
}

}  // namespace radray::render
