#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <process.h>
#endif

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <radray/file.h>
#include <radray/guid.h>
#include <radray/runtime/shader_binding_policy.h>
#include <radray/runtime/shader_parameters.h>
#include <radray/runtime/render_framework/forward_pipeline.h>

namespace radray {
namespace {

constexpr std::string_view kAssetId = "cfa4818c-64e7-47b2-981d-6aa70fdeeb5f";

string MakeForwardShaderManifest() {
    return fmt::format(R"json({{
  "AssetId": "{}",
  "Passes": [{{
    "Name": "Forward",
    "Source": "forward_pipeline/forward_pass.hlsl",
    "ShaderModel": "6_0",
    "Optimize": true,
    "EnableUnbounded": false,
    "Keywords": [
      {{"Scope": "local", "Stages": ["pixel"], "Alternatives": ["", "_BASECOLOR_MAP=1"]}},
      {{"Scope": "local", "Stages": ["pixel"], "Alternatives": ["", "_METALROUGHNESS_MAP=1"]}},
      {{"Scope": "local", "Stages": ["pixel"], "Alternatives": ["", "_NORMAL_MAP=1"]}},
      {{"Scope": "local", "Stages": ["pixel"], "Alternatives": ["", "_OCCLUSION_MAP=1"]}},
      {{"Scope": "local", "Stages": ["pixel"], "Alternatives": ["", "_EMISSIVE_MAP=1"]}}
    ],
    "Variants": [
      [],
      ["_BASECOLOR_MAP=1"],
      ["_METALROUGHNESS_MAP=1"],
      ["_NORMAL_MAP=1"],
      ["_OCCLUSION_MAP=1"],
      ["_EMISSIVE_MAP=1"]
    ],
    "Program": {{
      "Type": "graphics",
      "Vertex": "VSMain",
      "Pixel": "PSMain",
      "ColorTargets": [{{"Index": 0}}]
    }}
  }}, {{
    "Name": "Shadow",
    "Source": "forward_pipeline/shadow_pass.hlsl",
    "ShaderModel": "6_0",
    "Optimize": true,
    "EnableUnbounded": false,
    "Keywords": [
      {{"Scope": "local", "Stages": ["vertex"], "Alternatives": ["", "_POINT_SHADOW_LAYERED=1"]}}
    ],
    "Variants": [[], ["_POINT_SHADOW_LAYERED=1"]],
    "Program": {{
      "Type": "graphics",
      "Vertex": "VSMain",
      "Pixel": "PSMain",
      "ColorTargets": []
    }}
  }}]
}})json",
                       kAssetId);
}

int RunCooker(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    std::filesystem::path shaderRoot = {}) {
    const string executable = RADRAY_SHADER_COOKER_PATH;
    const string inputString = input.string();
    const string outputString = output.string();
    if (shaderRoot.empty()) shaderRoot = std::filesystem::path{RADRAY_PROJECT_DIR} / "shaderlib";
    const string rootString = shaderRoot.string();
#ifdef _WIN32
    const std::array<const char*, 10> arguments{
        executable.c_str(),
        "--input", inputString.c_str(),
        "--output", outputString.c_str(),
        "--shader-root", rootString.c_str(),
        "--target", "all",
        nullptr};
    return static_cast<int>(_spawnv(_P_WAIT, executable.c_str(), arguments.data()));
#else
    const string command = fmt::format(
        "'{}' --input '{}' --output '{}' --shader-root '{}' --target all",
        executable, inputString, outputString, rootString);
    return std::system(command.c_str());
#endif
}

constexpr std::string_view kProjectedConstantsShader = R"hlsl(
#ifdef VULKAN
#define RR_BINDING(b, s) [[vk::binding(b, s)]]
#else
#define RR_BINDING(b, s)
#endif

struct MaterialConstants {
    float4 First;
    float4 Second;
};

RR_BINDING(0, 2) ConstantBuffer<MaterialConstants> gMaterial : register(b0, space2);

float4 VSMain(float4 position : POSITION) : SV_Position {
    return position;
}

float4 PSMain() : SV_Target0 {
#if USE_SECOND
    return gMaterial.Second;
#else
    return gMaterial.First;
#endif
}
)hlsl";

constexpr std::string_view kProjectedConstantsManifest = R"json({
  "AssetId": "df48bf44-42ef-4506-9ea7-0b4af53cac02",
  "Passes": [{
    "Name": "ProjectedConstants",
    "Source": "projection.hlsl",
    "ShaderModel": "6_0",
    "Optimize": true,
    "EnableUnbounded": false,
    "Keywords": [
      {"Scope": "local", "Stages": ["pixel"], "Alternatives": ["", "USE_SECOND=1"]}
    ],
    "BakeSet": [[], ["USE_SECOND=1"]],
    "Program": {
      "Type": "graphics",
      "Vertex": "VSMain",
      "Pixel": "PSMain",
      "ColorTargets": [{"Index": 0}]
    }
  }]
})json";

}  // namespace

TEST(ForwardMaterialCookerTest, BuildsRuntimeLayoutAcrossBackendsAndVariants) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / fmt::format("radray_forward_material_{}", Guid::NewGuid());
    const std::filesystem::path input = directory / "forward.shader.json";
    const std::filesystem::path output = directory / "forward.shader.bin";
    ASSERT_TRUE(WriteTextFile(input, MakeForwardShaderManifest()));
    ASSERT_EQ(RunCooker(input, output), 0);

    auto binary = render::ReadShaderBinary(output);
    ASSERT_TRUE(binary.has_value());
    for (const render::ShaderPassDesc& pass : binary->Asset.Passes) {
        EXPECT_NE(pass.SourceIdentity, render::ShaderHash{});
    }
    ForwardPipeline pipeline;
    auto layout = BuildShaderParameterLayout(
        *binary,
        pipeline.GetShaderBindingPolicy(),
        render::ShaderProgramKind::Graphics);
    ASSERT_TRUE(layout.Succeeded());
    ASSERT_TRUE(layout.Layout->IsValid());
    ASSERT_EQ(layout.Layout->Bindings().size(), 7u);
    EXPECT_TRUE(layout.Layout->FindBinding("gMaterial").HasValue());
    EXPECT_TRUE(layout.Layout->FindBinding("gBaseColorMap").HasValue());
    EXPECT_TRUE(layout.Layout->FindBinding("gMetalRoughMap").HasValue());
    EXPECT_TRUE(layout.Layout->FindBinding("gNormalMap").HasValue());
    EXPECT_TRUE(layout.Layout->FindBinding("gOcclusionMap").HasValue());
    EXPECT_TRUE(layout.Layout->FindBinding("gEmissiveMap").HasValue());
    EXPECT_TRUE(layout.Layout->FindBinding("gSampler").HasValue());

    const render::ShaderInterfaceDesc& sourceInterface = binary->ProgramInterfaces.front();
    render::ShaderInterfaceDesc wrongObjectLayout = sourceInterface;
    auto objectGroup = std::ranges::find(
        wrongObjectLayout.BindingGroups,
        ForwardPipeline::kObjectBindingGroup,
        &render::ShaderBindingGroupInterfaceDesc::GroupIndex);
    ASSERT_NE(objectGroup, wrongObjectLayout.BindingGroups.end());
    ASSERT_EQ(objectGroup->Bindings.size(), 1u);
    ASSERT_TRUE(objectGroup->Bindings.front().Buffer.has_value());
    ASSERT_EQ(objectGroup->Bindings.front().Buffer->Fields.size(), 1u);
    objectGroup->Bindings.front().Buffer->Fields.front().Type.RowMajor =
        !objectGroup->Bindings.front().Buffer->Fields.front().Type.RowMajor;
    auto wrongObject = ResolveShaderBindings(
        wrongObjectLayout,
        pipeline.GetShaderBindingPolicy());
    ASSERT_FALSE(wrongObject.Succeeded());
    ASSERT_EQ(wrongObject.Diagnostics.size(), 1u);
    EXPECT_EQ(wrongObject.Diagnostics.front().Context.Group, ForwardPipeline::kObjectBindingGroup);
    EXPECT_EQ(wrongObject.Diagnostics.front().Context.Binding, 1u);

    render::ShaderInterfaceDesc shadowTextureInterface = sourceInterface;
    auto pipelineGroup = std::ranges::find(
        shadowTextureInterface.BindingGroups,
        ForwardPipeline::kPipelineBindingGroup,
        &render::ShaderBindingGroupInterfaceDesc::GroupIndex);
    ASSERT_NE(pipelineGroup, shadowTextureInterface.BindingGroups.end());
    pipelineGroup->Bindings.emplace_back(render::ShaderBindingDesc{
        .Name = "CustomShadowCube",
        .BindingIndex = 1,
        .Kind = render::ShaderBindingKind::SampledTexture,
        .Access = render::ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Stages = render::ShaderStage::Pixel,
        .Texture = render::ShaderTextureInterfaceDesc{
            .Dimension = render::ShaderTextureDimension::Cube,
            .SampleType = render::ShaderSampleType::Float}});
    ASSERT_TRUE(ResolveShaderBindings(
                    shadowTextureInterface,
                    pipeline.GetShaderBindingPolicy())
                    .Succeeded());
    pipelineGroup->Bindings.back().Texture->Dimension = render::ShaderTextureDimension::Dim2D;
    auto wrongTexture = ResolveShaderBindings(
        shadowTextureInterface,
        pipeline.GetShaderBindingPolicy());
    ASSERT_FALSE(wrongTexture.Succeeded());
    EXPECT_EQ(wrongTexture.Diagnostics.front().Context.Binding, 1u);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(ForwardMaterialCookerTest, BuildsConstantFieldUnionAcrossRealVariantsAndTargets) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        fmt::format("radray_projected_constants_{}", Guid::NewGuid());
    const std::filesystem::path input = directory / "projection.shader.json";
    const std::filesystem::path source = directory / "projection.hlsl";
    const std::filesystem::path output = directory / "projection.shader.bin";
    ASSERT_TRUE(WriteTextFile(input, kProjectedConstantsManifest));
    ASSERT_TRUE(WriteTextFile(source, kProjectedConstantsShader));
    ASSERT_EQ(RunCooker(input, output, directory), 0);

    auto binary = render::ReadShaderBinary(output);
    ASSERT_TRUE(binary.has_value());
    ASSERT_TRUE(binary->IsBakeComplete(render::ShaderTarget::DXIL));
    ASSERT_TRUE(binary->IsBakeComplete(render::ShaderTarget::SPIRV));
    ASSERT_NE(binary->Asset.Passes.front().SourceIdentity, render::ShaderHash{});
    auto layout = BuildShaderParameterLayout(
        *binary,
        PipelineBindingPolicy{},
        render::ShaderProgramKind::Graphics);
    ASSERT_TRUE(layout.Succeeded());
    auto constants = layout.Layout->FindBinding({2, 0});
    ASSERT_TRUE(constants.HasValue());
    ASSERT_TRUE(constants.Get()->Interface.Buffer.has_value());
    EXPECT_EQ(constants.Get()->Interface.Buffer->ByteSize, 32u);
    EXPECT_TRUE(layout.Layout->FindField({2, 0}, "First").HasValue());
    EXPECT_TRUE(layout.Layout->FindField({2, 0}, "Second").HasValue());

    ShaderParameterSet parameters;
    ASSERT_TRUE(parameters.Reset(*layout.Layout));
    for (const render::ShaderProgramVariantArtifact& program : binary->ProgramVariants) {
        auto plan = ResolveShaderBindings(
            binary->ProgramInterfaces[program.InterfaceIndex],
            PipelineBindingPolicy{});
        ASSERT_TRUE(plan.Succeeded());
        EXPECT_TRUE(parameters.IsCompleteFor(*plan.Plan));
    }

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

}  // namespace radray
