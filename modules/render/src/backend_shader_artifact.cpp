#include <radray/render/backend_shader_artifact.h>

#include <utility>

#if defined(RADRAY_ENABLE_D3D12)
#include <radray/render/backend/d3d12_impl.h>
#endif
#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/render/backend/vulkan_impl.h>
#endif

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

BackendShaderArtifact::BackendShaderArtifact(
    shader::DxilShaderArtifactView artifact,
    unique_ptr<PipelineLayout> layout) noexcept
    : _artifact{std::move(artifact)},
      Layout{std::move(layout)},
      Category{ShaderBlobCategory::DXIL} {}

BackendShaderArtifact::BackendShaderArtifact(
    shader::SpirvShaderArtifactView artifact,
    unique_ptr<PipelineLayout> layout) noexcept
    : _artifact{std::move(artifact)},
      Layout{std::move(layout)},
      Category{ShaderBlobCategory::SPIRV} {}

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

std::optional<BackendShaderArtifact> CreateBackendShaderArtifact(
    Device& device,
    std::span<const byte> blob,
    const shader::ShaderArtifactDecodeOptions& options,
    const ShaderLayoutPolicy& policy,
    BackendShaderArtifactError* error) noexcept {
    SetError(error, BackendShaderArtifactFailure::None);
    const std::optional<shader::ShaderTarget> backendTarget =
        GetShaderTargetForBackend(device.GetBackend());
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
#if defined(RADRAY_ENABLE_D3D12)
            std::optional<shader::DxilShaderArtifactView> artifact =
                shader::DecodeDxilShaderArtifact(blob, options, &decodeError);
            if (!artifact.has_value()) {
                SetError(error, BackendShaderArtifactFailure::DecodeFailed, decodeError);
                return std::nullopt;
            }
            Nullable<unique_ptr<PipelineLayout>> layout =
                static_cast<d3d12::DeviceD3D12&>(device).CreatePipelineLayout(artifact.value(), policy);
            if (!layout.HasValue()) {
                SetError(error, BackendShaderArtifactFailure::PipelineLayoutCreationFailed);
                return std::nullopt;
            }
            return BackendShaderArtifact{std::move(artifact.value()), layout.Release()};
#else
            SetError(error, BackendShaderArtifactFailure::UnsupportedBackend);
            return std::nullopt;
#endif
        }
        case shader::ShaderTarget::SPIRV: {
#if defined(RADRAY_ENABLE_VULKAN)
            std::optional<shader::SpirvShaderArtifactView> artifact =
                shader::DecodeSpirvShaderArtifact(blob, options, &decodeError);
            if (!artifact.has_value()) {
                SetError(error, BackendShaderArtifactFailure::DecodeFailed, decodeError);
                return std::nullopt;
            }
            Nullable<unique_ptr<PipelineLayout>> layout =
                static_cast<vulkan::DeviceVulkan&>(device).CreatePipelineLayout(artifact.value(), policy);
            if (!layout.HasValue()) {
                SetError(error, BackendShaderArtifactFailure::PipelineLayoutCreationFailed);
                return std::nullopt;
            }
            return BackendShaderArtifact{std::move(artifact.value()), layout.Release()};
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
