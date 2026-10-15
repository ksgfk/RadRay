#include <radray/render/backend_shader_artifact.h>

#include <utility>

namespace radray::render {
namespace {

void SetError(
    BackendShaderArtifactError* error,
    BackendShaderArtifactFailure failure,
    shader::ShaderArtifactDecodeError decodeFailure = shader::ShaderArtifactDecodeError::None) noexcept {
    if (error != nullptr) {
        error->Failure = failure;
        error->DecodeFailure = decodeFailure;
    }
}

}  // namespace

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

std::optional<PreparedBackendShaderArtifact> PrepareBackendShaderArtifact(
    RenderBackend backend,
    std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options,
    const ShaderProgramLayoutRecipe& recipe,
    Nullable<BackendShaderArtifactError*> outError) noexcept {
    auto* error = outError.HasValue() ? outError.Get() : nullptr;
    SetError(error, BackendShaderArtifactFailure::None);
    const std::optional<shader::ShaderTarget> backendTarget = GetShaderTargetForBackend(backend);
    if (!backendTarget.has_value()) {
        SetError(error, BackendShaderArtifactFailure::UnsupportedBackend);
        return std::nullopt;
    }
    if (backendTarget.value() != options.Target) {
        SetError(error, BackendShaderArtifactFailure::TargetMismatch);
        return std::nullopt;
    }
    shader::ShaderArtifactDecodeError decodeError = shader::ShaderArtifactDecodeError::None;
    switch (options.Target) {
        case shader::ShaderTarget::DXIL: {
            std::optional<shader::DxilShaderArtifactView> artifact =
                shader::DecodeDxilShaderArtifact(blob, options, &decodeError);
            if (!artifact.has_value()) {
                SetError(error, BackendShaderArtifactFailure::DecodeFailed, decodeError);
                return std::nullopt;
            }
            std::optional<ResolvedD3D12Layout> resolved =
                ResolveD3D12Layout(artifact.value(), recipe.D3D12);
            if (!resolved.has_value()) {
                SetError(error, BackendShaderArtifactFailure::LayoutResolveFailed);
                return std::nullopt;
            }
            return PreparedBackendShaderArtifact{std::move(*artifact), std::move(*resolved)};
        }
        case shader::ShaderTarget::SPIRV: {
            std::optional<shader::SpirvShaderArtifactView> artifact =
                shader::DecodeSpirvShaderArtifact(blob, options, &decodeError);
            if (!artifact.has_value()) {
                SetError(error, BackendShaderArtifactFailure::DecodeFailed, decodeError);
                return std::nullopt;
            }
            std::optional<ResolvedVulkanLayout> resolved =
                ResolveVulkanLayout(artifact.value(), recipe.Vulkan);
            if (!resolved.has_value()) {
                SetError(error, BackendShaderArtifactFailure::LayoutResolveFailed);
                return std::nullopt;
            }
            return PreparedBackendShaderArtifact{std::move(*artifact), std::move(*resolved)};
        }
    }
    SetError(error, BackendShaderArtifactFailure::TargetMismatch);
    return std::nullopt;
}

std::optional<ResolvedLayoutHash> ResolveBackendLayoutHash(
    RenderBackend backend, std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options, const ShaderProgramLayoutRecipe& recipe,
    BackendShaderArtifactError* error) noexcept {
    auto prepared = PrepareBackendShaderArtifact(backend, blob, options, recipe, error);
    if (!prepared) return std::nullopt;
    return prepared->LayoutHash();
}

std::optional<BackendShaderArtifact> CreateBackendShaderArtifact(
    Device& device, std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options, const ShaderProgramLayoutRecipe& recipe,
    BackendShaderArtifactError* error) noexcept {
    auto prepared = PrepareBackendShaderArtifact(device.GetBackend(), blob, options, recipe, error);
    if (!prepared) return std::nullopt;
    return CreateBackendShaderArtifact(device, std::move(*prepared), error);
}

std::optional<BackendShaderArtifact> CreateBackendShaderArtifact(
    Device& device,
    PreparedBackendShaderArtifact prepared,
    Nullable<BackendShaderArtifactError*> outError) noexcept {
    auto* error = outError.HasValue() ? outError.Get() : nullptr;
    SetError(error, BackendShaderArtifactFailure::None);
    const std::optional<shader::ShaderTarget> backendTarget =
        GetShaderTargetForBackend(device.GetBackend());
    if (!backendTarget.has_value()) {
        SetError(error, BackendShaderArtifactFailure::UnsupportedBackend);
        return std::nullopt;
    }
    if (backendTarget.value() != prepared.GetTarget()) {
        SetError(error, BackendShaderArtifactFailure::TargetMismatch);
        return std::nullopt;
    }

    switch (prepared.GetTarget()) {
        case shader::ShaderTarget::DXIL: {
#if defined(RADRAY_ENABLE_D3D12)
            auto& artifact = std::get<shader::DxilShaderArtifactView>(prepared._artifact);
            auto& resolved = std::get<ResolvedD3D12Layout>(prepared._layout);
            auto* factory = dynamic_cast<detail::D3D12PipelineLayoutFactory*>(&device);
            Nullable<unique_ptr<PipelineLayout>> layout = factory ? factory->CreatePipelineLayout(resolved) : nullptr;
            if (!layout.HasValue()) {
                SetError(error, BackendShaderArtifactFailure::PipelineLayoutCreationFailed);
                return std::nullopt;
            }
            return BackendShaderArtifact{
                std::move(artifact),
                std::move(resolved),
                layout.Release()};
#else
            SetError(error, BackendShaderArtifactFailure::UnsupportedBackend);
            return std::nullopt;
#endif
        }
        case shader::ShaderTarget::SPIRV: {
#if defined(RADRAY_ENABLE_VULKAN)
            auto& artifact = std::get<shader::SpirvShaderArtifactView>(prepared._artifact);
            auto& resolved = std::get<ResolvedVulkanLayout>(prepared._layout);
            auto* factory = dynamic_cast<detail::VulkanPipelineLayoutFactory*>(&device);
            Nullable<unique_ptr<PipelineLayout>> layout = factory ? factory->CreatePipelineLayout(resolved) : nullptr;
            if (!layout.HasValue()) {
                SetError(error, BackendShaderArtifactFailure::PipelineLayoutCreationFailed);
                return std::nullopt;
            }
            return BackendShaderArtifact{
                std::move(artifact),
                std::move(resolved),
                layout.Release()};
#else
            SetError(error, BackendShaderArtifactFailure::UnsupportedBackend);
            return std::nullopt;
#endif
        }
    }
    SetError(error, BackendShaderArtifactFailure::TargetMismatch);
    return std::nullopt;
}

}  // namespace radray::render
