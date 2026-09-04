#pragma once

#include <radray/render/backend_shader_artifact.h>
#include <radray/runtime/shader_jit.h>
#include <radray/runtime/shader_program.h>

namespace radray::test {

struct FoundationComputeProgram {
    unique_ptr<render::PipelineLayout> Layout;
    unique_ptr<render::Shader> Shader;
    unique_ptr<render::ComputePipelineState> Pso;
};

inline Nullable<unique_ptr<ShaderProgram>> CompileFoundationGraphics(render::Device& device, std::string_view source) {
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
    auto backend = render::CreateBackendShaderArtifact(device, artifact->Metadata, {*target, artifact->ExpectedGpuArtifact});
    return backend ? ShaderProgram::Create(&device, std::move(*backend)) : nullptr;
}

inline std::optional<FoundationComputeProgram> CompileFoundationCompute(render::Device& device, std::string_view source) {
    ShaderJit jit{{std::filesystem::path{RADRAY_PROJECT_DIR} / "shaderlib"}};
    const auto target = render::GetShaderTargetForBackend(device.GetBackend());
    if (!jit.IsAvailable() || !target) return std::nullopt;
    const auto bytes = std::as_bytes(std::span{source.data(), source.size()});
    const auto contract = jit.DiscoverContractHash("tests/foundation_compute.hlsl", bytes, *target);
    if (!contract) return std::nullopt;
    shader::CompileVariantRequest request{};
    request.SourceName = "tests/foundation_compute.hlsl";
    request.RootSource = {bytes.begin(), bytes.end()};
    request.Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(*target));
    request.ExpectedContract = *contract;
    const auto artifact = jit.Compile(request, *target);
    if (!artifact) return std::nullopt;
    auto backend = render::CreateBackendShaderArtifact(device, artifact->Metadata, {*target, artifact->ExpectedGpuArtifact});
    if (!backend) return std::nullopt;
    const auto bytecode = backend->Generic().FindStageBytecode(shader::ShaderStage::Compute);
    if (!bytecode) return std::nullopt;
    auto shader = device.CreateShader({*bytecode, backend->Category, render::ShaderStage::Compute});
    if (!shader) return std::nullopt;
    FoundationComputeProgram result;
    result.Layout = std::move(backend->Layout);
    result.Shader = shader.Release();
    auto pso = device.CreateComputePipelineState({result.Layout.get(), {result.Shader.get(), "CSMain"}});
    if (!pso) return std::nullopt;
    result.Pso = pso.Release();
    return result;
}

}  // namespace radray::test
