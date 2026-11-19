#include "../../../tools/shader_reflect/shader_reflect.h"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <radray/file.h>
#include <radray/json.h>
#include <radray/shader/shader_layout.h>
#include <radray/shader/shader_compiler_contract.h>
#include <radray/shader/shader_reflector.h>
#include <radray/shader/client.h>

#include "compiler_test_utils.h"

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

TEST_F(ShaderReflectCli, SupportsPromoteDxilCBufferAndAllowDxilOnlyInManifestPolicy) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    constexpr std::string_view kSource = R"hlsl(
#include <core/platform.hlsli>
struct SharedPushData {
    uint A;
    uint B;
};
VK_PUSH_CONSTANT ConstantBuffer<SharedPushData> SharedPush : register(b0, space2);
#if !defined(__spirv__)
struct AuxPushData {
    uint BaseInstance;
};
ConstantBuffer<AuxPushData> DxAux : register(b1, space2);
#endif
VK_BINDING(0, 0) RWStructuredBuffer<uint> OutBuf : register(u0, space0);

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain() {
#if !defined(__spirv__)
    OutBuf[0] = SharedPush.A + SharedPush.B + DxAux.BaseInstance;
#else
    OutBuf[0] = SharedPush.A + SharedPush.B;
#endif
}
)hlsl";
    auto compiled = test::DiscoverAndCompile(client, {.SourceName = "reflect_cli/promoted_push.hlsl", .Source = test::ToBytes(kSource), .Targets = ShaderTargetMask::All});
    ASSERT_TRUE(compiled.Succeeded()) << compiled.Diagnostics;

    vector<byte> dxBytes, vkBytes;
    WireMetadataEnvelope dxEnv{}, vkEnv{};
    for (const auto& lane : *compiled.Value) {
        if (lane.Target == ShaderTarget::DXIL) {
            dxBytes = lane.Metadata;
            ASSERT_TRUE(DecodeShaderMetadataEnvelope(std::span<const byte>{dxBytes}.first(152), dxEnv));
            ASSERT_TRUE(WriteBinaryFile(Directory / "promoted.dxil.bin", dxBytes));
        } else {
            vkBytes = lane.Metadata;
            ASSERT_TRUE(DecodeShaderMetadataEnvelope(std::span<const byte>{vkBytes}.first(152), vkEnv));
            ASSERT_TRUE(WriteBinaryFile(Directory / "promoted.spirv.bin", vkBytes));
        }
    }

    auto writeManifest = [&](bool badBool) {
        JsonWriter writer;
        auto root = writer.RootObject();
        root.AddUint("version", 1);
        auto policy = root.AddObject("policy");
        policy.AddBool("allowTargetOnlyBindings", false);
        auto pushes = policy.AddArray("pushConstants");
        auto first = pushes.AppendObject();
        first.AddString("name", "SharedPush");
        first.AddUint("pushId", 1);
        if (badBool)
            first.AddString("promoteDxilCBufferToRootConstants", "yes");
        else
            first.AddBool("promoteDxilCBufferToRootConstants", true);
        auto second = pushes.AppendObject();
        second.AddString("name", "DxAux");
        second.AddUint("pushId", 2);
        second.AddBool("promoteDxilCBufferToRootConstants", true);
        second.AddBool("allowDxilOnly", true);

        auto variant = root.AddArray("variants").AppendObject();
        variant.AddString("name", "Default");
        auto dx = variant.AddObject("dxil");
        dx.AddString("path", "promoted.dxil.bin");
        dx.AddString("gpuArtifactHash", dxEnv.GpuArtifact.ToHex());
        dx.AddUint("toolchainIdentity", dxEnv.ToolchainIdentity);
        auto vk = variant.AddObject("spirv");
        vk.AddString("path", "promoted.spirv.bin");
        vk.AddString("gpuArtifactHash", vkEnv.GpuArtifact.ToHex());
        vk.AddUint("toolchainIdentity", vkEnv.ToolchainIdentity);
        ASSERT_TRUE(writer.WriteFile(Directory / "input.json"));
    };

    writeManifest(true);
    EXPECT_NE(Run(), 0);

    writeManifest(false);
    ASSERT_EQ(Run(), 0);

    auto dxDecoded = DecodeDxilShaderArtifact(dxBytes, {ShaderTarget::DXIL, dxEnv.GpuArtifact, kShaderToolchainIdentity});
    auto vkDecoded = DecodeSpirvShaderArtifact(vkBytes, {ShaderTarget::SPIRV, vkEnv.GpuArtifact, kShaderToolchainIdentity});
    ASSERT_TRUE(dxDecoded.Succeeded() && vkDecoded.Succeeded());
    ShaderReflectionPolicy expectedPolicy;
    expectedPolicy.PushConstants.push_back({.Name = "SharedPush", .PushId = 1, .PromoteDxilCBufferToRootConstants = true});
    expectedPolicy.PushConstants.push_back({.Name = "DxAux", .PushId = 2, .PromoteDxilCBufferToRootConstants = true, .AllowDxilOnly = true});
    const ShaderVariantArtifacts variant{"Default", &*dxDecoded.Value, &*vkDecoded.Value};
    const auto online = ReflectShaderFamily(std::span{&variant, size_t{1}}, expectedPolicy);
    ASSERT_TRUE(online.Succeeded()) << online.Diagnostics;
    const auto expectedJson = WriteShaderFamilyLayoutJson(*online.Value);
    ASSERT_TRUE(expectedJson);
    const auto cliJsonBytes = ReadBinaryFile(Directory / "result.layout.json");
    ASSERT_TRUE(cliJsonBytes);
    const string cliJson{reinterpret_cast<const char*>(cliJsonBytes->data()), cliJsonBytes->size()};
    EXPECT_EQ(cliJson, *expectedJson);
}

}  // namespace
}  // namespace radray::shader
