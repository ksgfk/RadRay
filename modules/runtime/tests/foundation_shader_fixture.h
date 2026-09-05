#pragma once

#include <radray/render/backend_shader_artifact.h>
#include <radray/runtime/shader_jit.h>
#include <radray/runtime/shader_program.h>

namespace radray::test {

inline Nullable<unique_ptr<ShaderProgram>> CompileFoundationGraphics(
    render::Device& device, std::string_view source,
    const render::ShaderProgramLayoutRecipe& recipe = {}) {
    ShaderJit jit{{std::filesystem::path{RADRAY_PROJECT_DIR} / "shaderlib"}};
    const auto target = render::GetShaderTargetForBackend(device.GetBackend());
    if (!jit.IsAvailable() || !target) return nullptr;
    const auto bytes = std::as_bytes(std::span{source.data(), source.size()});
    const auto contract = jit.DiscoverContractHash("tests/foundation_graphics.hlsl", bytes, *target);
    if (!contract) return nullptr;
    shader::CompileVariantRequest request{};
    request.SourceName = "tests/foundation_graphics.hlsl";
    request.RootSource = {bytes.begin(), bytes.end()};
    request.Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(*target));
    request.ExpectedContract = *contract;
    auto artifact = jit.Compile(request, *target);
    if (!artifact) return nullptr;
    auto backend = render::CreateBackendShaderArtifact(
        device, artifact->Metadata, {*target, artifact->ExpectedGpuArtifact}, recipe);
    return backend ? ShaderProgram::Create(&device, std::move(*backend)) : nullptr;
}

inline Nullable<unique_ptr<ShaderProgram>> CompileFoundationCompute(
    render::Device& device, std::string_view source,
    const render::ShaderProgramLayoutRecipe& recipe = {}) {
    ShaderJit jit{{std::filesystem::path{RADRAY_PROJECT_DIR} / "shaderlib"}};
    const auto target = render::GetShaderTargetForBackend(device.GetBackend());
    if (!jit.IsAvailable() || !target) return nullptr;
    const auto bytes = std::as_bytes(std::span{source.data(), source.size()});
    const auto contract = jit.DiscoverContractHash("tests/foundation_compute.hlsl", bytes, *target);
    if (!contract) return nullptr;
    shader::CompileVariantRequest request{};
    request.SourceName = "tests/foundation_compute.hlsl";
    request.RootSource = {bytes.begin(), bytes.end()};
    request.Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(*target));
    request.ExpectedContract = *contract;
    const auto artifact = jit.Compile(request, *target);
    if (!artifact) return nullptr;
    auto backend = render::CreateBackendShaderArtifact(
        device, artifact->Metadata, {*target, artifact->ExpectedGpuArtifact}, recipe);
    return backend ? ShaderProgram::Create(&device, std::move(*backend)) : nullptr;
}

}  // namespace radray::test
