#pragma once

#include <radray/file.h>
#include <radray/runtime/shader_jit.h>
#include <radray/runtime/shader_program.h>

namespace radray::test {

inline Nullable<unique_ptr<ShaderProgram>> CompileStageBProgram(render::Device& device, std::string_view source,
                                                                const render::ShaderProgramLayoutRecipe& recipe = {}) {
    ShaderJit jit{{std::filesystem::path{RADRAY_PROJECT_DIR} / "shaderlib"}};
    const auto target = render::GetShaderTargetForBackend(device.GetBackend());
    if (!target || !jit.IsAvailable()) return nullptr;
    const auto bytes = std::as_bytes(std::span{source.data(), source.size()});
    const auto contract = jit.DiscoverContractHash("stage_b_test.hlsl", bytes, *target);
    if (!contract) return nullptr;
    shader::CompileVariantRequest request{
        .SourceName = "stage_b_test.hlsl", .RootSource = {bytes.begin(), bytes.end()}, .Defines = {}, .Assignments = {}, .Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(*target)), .ExpectedContract = *contract};
    const auto compiled = jit.Compile(request, *target);
    if (!compiled) return nullptr;
    auto artifact = render::CreateBackendShaderArtifact(device, compiled->Metadata, {.Target = *target, .ExpectedGpuArtifact = compiled->ExpectedGpuArtifact}, recipe);
    return artifact ? ShaderProgram::Create(&device, std::move(*artifact)) : nullptr;
}

inline string StageBMaterialSource(std::string_view fields = "float4 BaseColor;", uint32_t group = 1,
                                   bool albedo = false, bool normal = false) {
    string source = "#include <core/platform.hlsli>\nstruct Values { " + string{fields} + " };\n";
    source += fmt::format("VK_BINDING(0, {}) ConstantBuffer<Values> MaterialValues : register(b0, space{});\n", group, group);
    if (albedo || normal) source += fmt::format("VK_BINDING(3, {}) SamplerState LinearSampler : register(s0, space{});\n", group, group);
    if (albedo) source += fmt::format("VK_BINDING(1, {}) Texture2D<float4> AlbedoTexture : register(t0, space{});\n", group, group);
    if (normal) source += fmt::format("VK_BINDING(2, {}) Texture2D<float4> NormalTexture : register(t1, space{});\n", group, group);
    source += "[shader(\"vertex\")] float4 VSMain(float3 p : POSITION) : SV_Position { return float4(p, 1); }\n";
    source += "[shader(\"pixel\")] float4 PSMain() : SV_Target0 { return MaterialValues.BaseColor.xxxx";
    if (albedo) source += " * AlbedoTexture.SampleLevel(LinearSampler, float2(0,0), 0)";
    if (normal) source += " * NormalTexture.SampleLevel(LinearSampler, float2(0,0), 0)";
    source += "; }\n";
    return source;
}

}  // namespace radray::test
