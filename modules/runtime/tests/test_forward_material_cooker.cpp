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
#include <radray/runtime/material_layout.h>
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
  }}]
}})json",
                       kAssetId);
}

int RunCooker(
    const std::filesystem::path& input,
    const std::filesystem::path& output) {
    const string executable = RADRAY_SHADER_COOKER_PATH;
    const string inputString = input.string();
    const string outputString = output.string();
    const string rootString = (std::filesystem::path{RADRAY_PROJECT_DIR} / "shaderlib").string();
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

}  // namespace

TEST(ForwardMaterialCookerTest, BuildsRuntimeLayoutAcrossBackendsAndVariants) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / fmt::format("radray_forward_material_{}", Guid::NewGuid());
    const std::filesystem::path input = directory / "forward.shader.json";
    const std::filesystem::path output = directory / "forward.shader.bin";
    ASSERT_TRUE(WriteTextFile(input, MakeForwardShaderManifest()));
    ASSERT_EQ(RunCooker(input, output), 0);

    auto binary = shader::ReadShaderBinary(output);
    ASSERT_TRUE(binary.has_value());
    auto layout = BuildMaterialLayout(
        binary->Stages,
        ForwardPipeline::kMaterialBindingGroup);
    ASSERT_TRUE(layout.has_value());
    ASSERT_TRUE(layout->IsValid());
    ASSERT_EQ(layout->Bindings.size(), 7u);
    EXPECT_TRUE(layout->FindBinding("gMaterial").HasValue());
    EXPECT_TRUE(layout->FindBinding("gBaseColorMap").HasValue());
    EXPECT_TRUE(layout->FindBinding("gMetalRoughMap").HasValue());
    EXPECT_TRUE(layout->FindBinding("gNormalMap").HasValue());
    EXPECT_TRUE(layout->FindBinding("gOcclusionMap").HasValue());
    EXPECT_TRUE(layout->FindBinding("gEmissiveMap").HasValue());
    EXPECT_TRUE(layout->FindBinding("gSampler").HasValue());

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

}  // namespace radray
