#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#include <process.h>
#endif

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <radray/file.h>
#include <radray/guid.h>
#include <radray/shader/shader_binary.h>

using namespace radray;
using namespace radray::shader;

namespace {

constexpr std::string_view kAssetId = "f03d7516-40d4-4e48-8655-f46f053ee891";

string MakeManifest(
    bool validKeyword,
    std::string_view vertexEntry = "VSMain",
    std::string_view source = "forward_pipeline/error_pass.hlsl") {
    return fmt::format(R"json({{
  "AssetId": "{}",
  "Passes": [{{
    "Name": "CookerTest",
    "Source": "{}",
    "ShaderModel": "6_0",
    "Optimize": false,
    "EnableUnbounded": false,
    "Keywords": [{{
      "Scope": "local",
      "Stages": ["vertex", "pixel"],
      "Alternatives": ["", "USE_TEST=1"]
    }}],
    "Variants": [[], ["{}"]],
    "Program": {{
      "Type": "graphics",
      "Vertex": "{}",
      "Pixel": "PSMain",
      "ColorTargets": [{{"Index": 0}}]
    }}
  }}]
}})json",
        kAssetId,
        source,
        validKeyword ? "USE_TEST=1" : "UNKNOWN=1",
        vertexEntry);
}

int RunCooker(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    std::string_view target) {
    const string executable = RADRAY_SHADER_COOKER_PATH;
    const string inputString = input.string();
    const string outputString = output.string();
    const string rootString = (std::filesystem::path{RADRAY_PROJECT_DIR} / "shaderlib").string();
    const string targetString{target};
#ifdef _WIN32
    const std::array<const char*, 10> arguments{
        executable.c_str(),
        "--input", inputString.c_str(),
        "--output", outputString.c_str(),
        "--shader-root", rootString.c_str(),
        "--target", targetString.c_str(),
        nullptr};
    return static_cast<int>(_spawnv(_P_WAIT, executable.c_str(), arguments.data()));
#else
    const string command = fmt::format(
        "'{}' --input '{}' --output '{}' --shader-root '{}' --target {}",
        executable, inputString, outputString, rootString, targetString);
    return std::system(command.c_str());
#endif
}

}  // namespace

TEST(ShaderCookerTest, ProducesDxilAndSpirvPartitions) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / fmt::format("radray_shader_cooker_{}", Guid::NewGuid());
    const std::filesystem::path input = directory / "shader.json";
    const std::filesystem::path dxilOutput = directory / "shader.dxil.bin";
    ASSERT_TRUE(WriteTextFile(input, MakeManifest(true)));

    ASSERT_EQ(RunCooker(input, dxilOutput, "dxil"), 0);
    auto dxil = ReadShaderBinary(dxilOutput);
    ASSERT_TRUE(dxil.has_value());
    EXPECT_EQ(dxil->Asset.AssetId, Guid::Parse(kAssetId));
    EXPECT_EQ(dxil->Asset.Passes.front().Name, "CookerTest");
    EXPECT_TRUE(dxil->Find(ShaderTarget::DXIL, 0, ShaderStage::Vertex, {}).HasValue());
    EXPECT_TRUE(dxil->Find(ShaderTarget::DXIL, 0, ShaderStage::Pixel, {"USE_TEST=1"}).HasValue());
    EXPECT_FALSE(dxil->Find(ShaderTarget::SPIRV, 0, ShaderStage::Vertex, {}).HasValue());

#if defined(RADRAY_ENABLE_SPIRV_CROSS)
    const std::filesystem::path spirvOutput = directory / "shader.spirv.bin";
    ASSERT_EQ(RunCooker(input, spirvOutput, "spirv"), 0);
    auto spirv = ReadShaderBinary(spirvOutput);
    ASSERT_TRUE(spirv.has_value());
    EXPECT_TRUE(spirv->Find(ShaderTarget::SPIRV, 0, ShaderStage::Vertex, {}).HasValue());
    EXPECT_TRUE(spirv->Find(ShaderTarget::SPIRV, 0, ShaderStage::Pixel, {"USE_TEST=1"}).HasValue());
    EXPECT_FALSE(spirv->Find(ShaderTarget::DXIL, 0, ShaderStage::Vertex, {}).HasValue());

    const std::filesystem::path allOutput = directory / "shader.all.bin";
    ASSERT_EQ(RunCooker(input, allOutput, "all"), 0);
    auto all = ReadShaderBinary(allOutput);
    ASSERT_TRUE(all.has_value());
    EXPECT_TRUE(all->Find(ShaderTarget::DXIL, 0, ShaderStage::Vertex, {}).HasValue());
    EXPECT_TRUE(all->Find(ShaderTarget::SPIRV, 0, ShaderStage::Vertex, {}).HasValue());
#else
    const std::filesystem::path spirvOutput = directory / "shader.spirv.bin";
    EXPECT_NE(RunCooker(input, spirvOutput, "spirv"), 0);
    EXPECT_FALSE(std::filesystem::exists(spirvOutput));
#endif
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(ShaderCookerTest, RejectsUnknownKeywordWithoutReplacingOutput) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / fmt::format("radray_shader_cooker_invalid_{}", Guid::NewGuid());
    const std::filesystem::path input = directory / "shader.json";
    const std::filesystem::path output = directory / "shader.bin";
    ASSERT_TRUE(WriteTextFile(input, MakeManifest(true)));
    ASSERT_EQ(RunCooker(input, output, "dxil"), 0);
    const auto before = ReadBinaryFile(output);
    ASSERT_TRUE(before.has_value());

    ASSERT_TRUE(WriteTextFile(input, MakeManifest(false)));
    EXPECT_NE(RunCooker(input, output, "dxil"), 0);
    EXPECT_EQ(ReadBinaryFile(output), before);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(ShaderCookerTest, RejectsMalformedJsonAndCompileFailureWithoutOutput) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / fmt::format("radray_shader_cooker_fail_{}", Guid::NewGuid());
    const std::filesystem::path input = directory / "shader.json";
    const std::filesystem::path output = directory / "shader.bin";

    ASSERT_TRUE(WriteTextFile(input, "{"));
    EXPECT_NE(RunCooker(input, output, "dxil"), 0);
    EXPECT_FALSE(std::filesystem::exists(output));

    ASSERT_TRUE(WriteTextFile(input, MakeManifest(true, "MissingVertexEntry")));
    EXPECT_NE(RunCooker(input, output, "dxil"), 0);
    EXPECT_FALSE(std::filesystem::exists(output));

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}
