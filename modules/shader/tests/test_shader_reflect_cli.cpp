#include "../../../tools/shader_reflect/shader_reflect.h"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <radray/file.h>
#include <radray/json.h>
#include <radray/shader/shader_layout.h>
#include <radray/shader/shader_compiler_contract.h>

namespace radray::shader {
namespace {

class ShaderReflectCli : public testing::Test {
protected:
    void SetUp() override {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        Directory = std::filesystem::temp_directory_path() / radray::format("radray-reflect-{}", tick);
        std::error_code error;
        ASSERT_TRUE(std::filesystem::create_directory(Directory, error));
        ASSERT_FALSE(error);
    }
    void TearDown() override {
        std::error_code error;
        // Only this fixture's freshly created directory belongs to the test.
        if (Directory.parent_path() == std::filesystem::temp_directory_path()) std::filesystem::remove_all(Directory, error);
    }
    void Manifest(bool incorrectHash = false) {
        JsonWriter writer;
        auto root = writer.RootObject();
        root.AddUint("version", 1);
        auto variant = root.AddArray("variants").AppendObject();
        variant.AddString("name", "Default");
        for (bool dxil : {true, false}) {
            const auto name = dxil ? "input.dxil.bin" : "input.spirv.bin";
            const auto bytes = ReadBinaryFile(std::filesystem::path{RADRAY_TEST_SHADER_DIR} / "shader_artifacts" / (dxil ? "layout_constants.dxil.bin" : "layout_constants.spirv.bin"));
            ASSERT_TRUE(bytes);
            ASSERT_TRUE(WriteBinaryFile(Directory / name, *bytes));
            shader::WireMetadataEnvelope envelope{};
            ASSERT_GE(bytes->size(), sizeof(envelope));
            ASSERT_TRUE(shader::DecodeShaderMetadataEnvelope(std::span<const byte>{*bytes}.first(152), envelope));
            auto target = variant.AddObject(dxil ? "dxil" : "spirv");
            target.AddString("path", name);
            target.AddString("gpuArtifactHash", incorrectHash ? string(32, '0') : envelope.GpuArtifact.ToHex());
            target.AddUint("toolchainIdentity", envelope.ToolchainIdentity);
        }
        ASSERT_TRUE(writer.WriteFile(Directory / "input.json"));
    }
    int Run() {
        auto manifest = (Directory / "input.json").string();
        auto output = (Directory / "result").string();
        char executable[] = "radray_shader_reflect", manifestOption[] = "--manifest", outputOption[] = "--output";
        char* arguments[]{executable, manifestOption, manifest.data(), outputOption, output.data()};
        return RunShaderReflect(5, arguments);
    }
    std::filesystem::path Directory;
};

TEST_F(ShaderReflectCli, ExportsIndependentLayoutsAndStageIndexFromRelativePaths) {
    Manifest();
    ASSERT_EQ(Run(), 0);
    const auto bytes = ReadBinaryFile(Directory / "result.layout.bin");
    ASSERT_TRUE(bytes);
    ShaderFamilyLayoutDesc family;
    ASSERT_TRUE(ReadShaderFamilyLayoutBinary(*bytes, family));
    ASSERT_EQ(family.Variants.size(), 1u);
    const auto json = ReadBinaryFile(Directory / "result.layout.json");
    ASSERT_TRUE(json);
    ShaderFamilyLayoutDesc fromJson;
    ASSERT_TRUE(ReadShaderFamilyLayoutJson({reinterpret_cast<const char*>(json->data()), json->size()}, fromJson));
    EXPECT_EQ(WriteShaderFamilyLayoutBinary(fromJson), bytes);
    const auto index = JsonDocument::ParseFile(Directory / "result.index.json");
    ASSERT_TRUE(index);
    ASSERT_EQ(index->Root()["stages"].Size(), 2u);
    for (size_t i = 0; i < 2; ++i) {
        const auto stage = index->Root()["stages"].At(i);
        EXPECT_EQ(stage["entry"].AsString(), "CSMain");
        EXPECT_EQ(stage["stage"].AsString(), "compute");
        const auto code = ReadBinaryFile(Directory / stage["file"].AsString());
        ASSERT_TRUE(code);
        EXPECT_FALSE(code->empty());
    }
}

TEST_F(ShaderReflectCli, RejectsExternalHashMismatchBeforeWritingAnyOutput) {
    Manifest(true);
    EXPECT_NE(Run(), 0);
    EXPECT_FALSE(std::filesystem::exists(Directory / "result.index.json"));
    EXPECT_FALSE(std::filesystem::exists(Directory / "result.layout.bin"));
}

TEST_F(ShaderReflectCli, OutputFailureInvalidatesPreviousSuccessIndex) {
    Manifest();
    ASSERT_EQ(Run(), 0);
    ASSERT_TRUE(std::filesystem::remove(Directory / "result.layout.bin"));
    ASSERT_TRUE(std::filesystem::create_directory(Directory / "result.layout.bin"));
    EXPECT_NE(Run(), 0);
    EXPECT_FALSE(std::filesystem::exists(Directory / "result.index.json"));
}

}  // namespace
}  // namespace radray::shader
