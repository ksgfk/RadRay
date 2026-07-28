#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <radray/environment.h>
#include <radray/file.h>
#include <radray/shader/dxc.h>
#include <radray/shader/shader_manifest.h>

#include "shader_manifest_fixtures.h"

namespace radray {
namespace {

static_assert(json_serializable<ShaderAssetDesc>);
static_assert(json_deserializable<ShaderAssetDesc>);
static_assert(json_serializable<ShaderPassDesc>);
static_assert(json_deserializable<ShaderPassDesc>);
static_assert(json_serializable<ShaderArtifactIndex>);
static_assert(json_deserializable<ShaderArtifactIndex>);
static_assert(EnumContains(render::ShaderParameterBindingType::CBuffer));
static_assert(EnumContains(render::ShaderStage::Vertex));

using test::kForwardManifest;
using test::kImGuiManifest;
using test::kMinimalManifest;

ShaderAssetDesc ParseOk(std::string_view json) {
    ShaderAssetDiagnostic diag{};
    std::optional<ShaderAssetDesc> desc = ParseShaderAssetDesc(json, diag);
    EXPECT_TRUE(desc.has_value()) << diag.ToString();
    return desc.has_value() ? std::move(desc.value()) : ShaderAssetDesc{};
}

/// 断言解析失败, 并返回诊断供调用方检查上下文。
ShaderAssetDiagnostic ParseFail(std::string_view json) {
    ShaderAssetDiagnostic diag{};
    std::optional<ShaderAssetDesc> desc = ParseShaderAssetDesc(json, diag);
    EXPECT_FALSE(desc.has_value()) << "expected parse failure but it succeeded";
    EXPECT_FALSE(diag.Message.empty()) << "failure must carry a diagnostic message";
    return diag;
}

/// 把 kMinimalManifest 里的一段文本替换掉, 用于构造单点错误。
std::string Mutate(std::string_view base, std::string_view from, std::string_view to) {
    std::string text{base};
    const size_t pos = text.find(from);
    EXPECT_NE(pos, std::string::npos) << "mutation anchor not found: " << from;
    if (pos != std::string::npos) {
        text.replace(pos, from.size(), to);
    }
    return text;
}

// ==================== 正例: 解析 ====================

TEST(ShaderAssetTest, ParsesImGuiManifest) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    EXPECT_EQ(desc.Name, "RadRayImGui");
    EXPECT_EQ(desc.Source, "imgui/imgui_pass.hlsl");
    ASSERT_EQ(desc.Passes.size(), 1u);

    const ShaderPassDesc& pass = desc.Passes.front();
    EXPECT_EQ(pass.Name, "Default");
    EXPECT_EQ(pass.ShaderModel, render::HlslShaderModel::SM60);
    EXPECT_EQ(pass.GetStageMask(), render::ShaderStages{render::ShaderStage::Graphics});
    ASSERT_TRUE(pass.FindEntryPoint(render::ShaderStage::Vertex).has_value());
    EXPECT_EQ(pass.FindEntryPoint(render::ShaderStage::Vertex).value(), "VSMain");
    EXPECT_EQ(pass.FindEntryPoint(render::ShaderStage::Pixel).value(), "PSMain");
    EXPECT_FALSE(pass.FindEntryPoint(render::ShaderStage::Compute).has_value());

    ASSERT_TRUE(pass.PushConstant.has_value());
    EXPECT_EQ(pass.PushConstant->Name, "gPush");
    EXPECT_EQ(pass.PushConstant->Size, 16u);
    EXPECT_EQ(pass.PushConstant->Location.Group, 0u);
    EXPECT_EQ(pass.PushConstant->Location.Binding, 0u);

    // push constant 不占 descriptor set 槽位, 因此 group 0 完全不存在。
    EXPECT_FALSE(pass.FindGroup(0).HasValue());
    ASSERT_EQ(pass.BindingGroups.size(), 1u);
    EXPECT_EQ(pass.BindingGroups.front().Group, 1u);

    Nullable<const ShaderBindingDesc*> sampler = pass.FindBinding(1, 1);
    ASSERT_TRUE(sampler.HasValue());
    EXPECT_EQ(sampler.Unwrap()->Name, "gSampler");
    ASSERT_TRUE(sampler.Unwrap()->ImmutableSampler.has_value());
    EXPECT_EQ(sampler.Unwrap()->ImmutableSampler->MinFilter, render::FilterMode::Linear);
    // Count 未显式给出时默认为 1。
    EXPECT_EQ(sampler.Unwrap()->Count, 1u);
    // Residency 未显式给出时默认为 DescriptorTable。
    EXPECT_EQ(sampler.Unwrap()->Residency, ShaderBindingResidency::DescriptorTable);
}

TEST(ShaderAssetTest, ParsesForwardManifestWithKeywordGroups) {
    const ShaderAssetDesc desc = ParseOk(kForwardManifest);
    ASSERT_EQ(desc.KeywordGroups.size(), 4u);
    EXPECT_EQ(desc.KeywordGroups[0].Name, "BaseColorMap");
    EXPECT_TRUE(desc.KeywordGroups[0].IsOptional);
    EXPECT_EQ(desc.KeywordGroups[0].Stages, render::ShaderStages{render::ShaderStage::Pixel});
    ASSERT_EQ(desc.KeywordGroups[2].Keywords.size(), 2u);
    EXPECT_EQ(desc.KeywordGroups[2].Keywords[1], "_ALPHABLEND_ON");

    Nullable<const ShaderPassDesc*> passPtr = desc.FindPass("Forward");
    ASSERT_TRUE(passPtr.HasValue());
    const ShaderPassDesc& pass = *passPtr.Unwrap();
    EXPECT_FALSE(pass.PushConstant.has_value());
    ASSERT_EQ(pass.BindingGroups.size(), 3u);

    Nullable<const ShaderBindingDesc*> view = pass.FindBinding(1, 0);
    ASSERT_TRUE(view.HasValue());
    EXPECT_EQ(view.Unwrap()->Name, "gView");
    EXPECT_EQ(view.Unwrap()->Residency, ShaderBindingResidency::RootDescriptor);
}

TEST(ShaderAssetTest, RoundTripsThroughSerialization) {
    const ShaderAssetDesc original = ParseOk(kForwardManifest);
    std::optional<string> text = SerializeShaderAssetDesc(original);
    ASSERT_TRUE(text.has_value());

    ShaderAssetDiagnostic diag{};
    std::optional<ShaderAssetDesc> reparsed = ParseShaderAssetDesc(text.value(), diag);
    ASSERT_TRUE(reparsed.has_value()) << diag.ToString();
    EXPECT_EQ(original, reparsed.value());
}

TEST(ShaderAssetTest, RoundTripsImmutableSampler) {
    const ShaderAssetDesc original = ParseOk(kImGuiManifest);
    std::optional<string> text = SerializeShaderAssetDesc(original);
    ASSERT_TRUE(text.has_value());

    ShaderAssetDiagnostic diag{};
    std::optional<ShaderAssetDesc> reparsed = ParseShaderAssetDesc(text.value(), diag);
    ASSERT_TRUE(reparsed.has_value()) << diag.ToString();
    EXPECT_EQ(original, reparsed.value());
}

TEST(ShaderAssetTest, JsonCustomizationPointsRoundTripDirectly) {
    const std::optional<ShaderAssetDesc> decoded =
        DeserializeJson<ShaderAssetDesc>(kImGuiManifest);
    ASSERT_TRUE(decoded.has_value());

    const std::optional<string> json = SerializeJson(decoded.value(), false);
    ASSERT_TRUE(json.has_value());
    const std::optional<ShaderAssetDesc> reparsed =
        DeserializeJson<ShaderAssetDesc>(json.value());
    ASSERT_TRUE(reparsed.has_value());
    EXPECT_EQ(reparsed.value(), decoded.value());
}

TEST(ShaderAssetTest, ReflectionPayloadEnumsUseMemberNames) {
    render::HlslShaderDesc hlsl{};
    hlsl.MinFeatureLevel = render::HlslFeatureLevel::LEVEL12_1;
    const std::optional<string> hlslJson = render::SerializeHlslShaderDesc(hlsl);
    ASSERT_TRUE(hlslJson.has_value());
    const std::optional<JsonDocument> hlslDocument = JsonDocument::Parse(hlslJson.value());
    ASSERT_TRUE(hlslDocument.has_value());
    EXPECT_EQ(hlslDocument->Root()["MinFeatureLevel"].AsString(), "LEVEL12_1");

    const std::optional<render::HlslShaderDesc> decodedHlsl =
        render::DeserializeHlslShaderDesc(hlslJson.value());
    ASSERT_TRUE(decodedHlsl.has_value());
    EXPECT_EQ(decodedHlsl->MinFeatureLevel, render::HlslFeatureLevel::LEVEL12_1);

    render::SpirvShaderDesc spirv{};
    spirv.Types.push_back(render::SpirvTypeInfo{
        .Name = "float",
        .BaseType = render::SpirvBaseType::Float32,
    });
    const std::optional<string> spirvJson = render::SerializeSpirvShaderDesc(spirv);
    ASSERT_TRUE(spirvJson.has_value());
    const std::optional<JsonDocument> spirvDocument = JsonDocument::Parse(spirvJson.value());
    ASSERT_TRUE(spirvDocument.has_value());
    const JsonValue spirvTypes = spirvDocument->Root()["Types"];
    ASSERT_EQ(spirvTypes.Size(), 1u);
    EXPECT_EQ(spirvTypes.At(0)["BaseType"].AsString(), "Float32");

    const std::optional<render::SpirvShaderDesc> decodedSpirv =
        render::DeserializeSpirvShaderDesc(spirvJson.value());
    ASSERT_TRUE(decodedSpirv.has_value());
    ASSERT_EQ(decodedSpirv->Types.size(), 1u);
    EXPECT_EQ(decodedSpirv->Types.front().BaseType, render::SpirvBaseType::Float32);
}

TEST(ShaderAssetTest, EnumNamesAndStageFlagsUseMagicEnum) {
    EXPECT_EQ(
        EnumName(render::ShaderParameterBindingType::CBuffer),
        std::string_view{"CBuffer"});
    EXPECT_EQ(
        EnumCast<render::ShaderParameterBindingType>("Texture"),
        render::ShaderParameterBindingType::Texture);
    EXPECT_EQ(
        EnumCast<render::ShaderParameterBindingType>("DynamicCBuffer"),
        render::ShaderParameterBindingType::DynamicCBuffer);

    const render::ShaderStages graphics{render::ShaderStage::Graphics};
    const std::optional<string> json = SerializeJson(graphics, false);
    ASSERT_TRUE(json.has_value());
    EXPECT_EQ(json.value(), R"(["Vertex","Pixel"])");

    const std::optional<render::ShaderStages> decoded =
        DeserializeJson<render::ShaderStages>(json.value());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded.value(), graphics);
    EXPECT_FALSE(
        DeserializeJson<render::ShaderStages>(R"(["Geometry"])").has_value());
}

// layout 构建的用例已随 Build*Storage 移至
// modules/runtime/tests/test_shader_layout_binding.cpp。

// ==================== 负例: 格式与结构 ====================

TEST(ShaderAssetTest, RejectsUnknownFormatVersion) {
    const ShaderAssetDiagnostic diag =
        ParseFail(Mutate(kMinimalManifest, "\"FormatVersion\": 1", "\"FormatVersion\": 99"));
    EXPECT_NE(diag.Message.find("FormatVersion"), string::npos);
}

TEST(ShaderAssetTest, RejectsMissingFormatVersion) {
    ParseFail(R"JSON({"Name":"X","Source":"x.hlsl","Passes":[]})JSON");
}

TEST(ShaderAssetTest, RejectsMalformedJson) {
    ParseFail("{ this is not json");
}

TEST(ShaderAssetTest, RejectsAssetWithoutPasses) {
    ParseFail(R"JSON({"FormatVersion":1,"Name":"X","Source":"x.hlsl","Passes":[]})JSON");
}

TEST(ShaderAssetTest, RejectsPassWithoutSourceWhenAssetHasNoDefault) {
    const ShaderAssetDiagnostic diag =
        ParseFail(Mutate(kMinimalManifest, "\"Source\": \"minimal.hlsl\",", ""));
    EXPECT_EQ(diag.PassName, "Main");
    EXPECT_NE(diag.Message.find("Source"), string::npos);
}

TEST(ShaderAssetTest, RejectsUnknownEnumValue) {
    const ShaderAssetDiagnostic diag =
        ParseFail(Mutate(kMinimalManifest, "\"Type\": \"CBuffer\"", "\"Type\": \"Cbuffer\""));
    EXPECT_EQ(diag.BindingName, "gInput");
    EXPECT_NE(diag.Message.find("Cbuffer"), string::npos);
}

// 作者不应能直接写 Dynamic* —— 驻留方式由正交的 Residency 表达。
TEST(ShaderAssetTest, RejectsDynamicBindingTypeNames) {
    ParseFail(Mutate(kMinimalManifest, "\"Type\": \"CBuffer\"", "\"Type\": \"DynamicCBuffer\""));
}

TEST(ShaderAssetTest, RejectsGraphicsPassWithoutVertexStage) {
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        kMinimalManifest,
        R"("Stages": [{ "Stage": "Compute", "EntryPoint": "CSMain" }])",
        R"("Stages": [{ "Stage": "Pixel", "EntryPoint": "PSMain" }])"));
    EXPECT_NE(diag.Message.find("Vertex"), string::npos);
}

TEST(ShaderAssetTest, RejectsMixingComputeWithGraphicsStages) {
    ParseFail(Mutate(
        kMinimalManifest,
        R"("Stages": [{ "Stage": "Compute", "EntryPoint": "CSMain" }])",
        R"("Stages": [{ "Stage": "Compute", "EntryPoint": "CSMain" },
                      { "Stage": "Vertex", "EntryPoint": "VSMain" }])"));
}

TEST(ShaderAssetTest, RejectsDuplicateStage) {
    ParseFail(Mutate(
        kMinimalManifest,
        R"("Stages": [{ "Stage": "Compute", "EntryPoint": "CSMain" }])",
        R"("Stages": [{ "Stage": "Compute", "EntryPoint": "A" },
                      { "Stage": "Compute", "EntryPoint": "B" }])"));
}

TEST(ShaderAssetTest, RejectsComputePassWithVertexInput) {
    ParseFail(Mutate(
        kMinimalManifest,
        R"("BindingGroups": [)",
        R"("VertexInput": { "Buffers": [{"Binding":0,"ArrayStride":4}],
              "Attributes": [{"Semantic":"POSITION","Format":"FLOAT32"}] },
           "BindingGroups": [)"));
}

// ==================== 负例: 绑定 ABI ====================

TEST(ShaderAssetTest, RejectsZeroBindingCount) {
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        kMinimalManifest,
        R"("Type": "CBuffer", "Stages": ["Compute"])",
        R"("Type": "CBuffer", "Count": 0, "Stages": ["Compute"])"));
    EXPECT_NE(diag.Message.find("Count"), string::npos);
    EXPECT_EQ(diag.BindingName, "gInput");
}

TEST(ShaderAssetTest, RejectsEmptyBindingStages) {
    ParseFail(Mutate(kMinimalManifest, R"("Stages": ["Compute"])", R"("Stages": [])"));
}

TEST(ShaderAssetTest, RejectsBindingStageNotDeclaredByPass) {
    const ShaderAssetDiagnostic diag =
        ParseFail(Mutate(kMinimalManifest, R"("Stages": ["Compute"])", R"("Stages": ["Pixel"])"));
    EXPECT_NE(diag.Message.find("stage"), string::npos);
}

TEST(ShaderAssetTest, RejectsRootDescriptorOnTexture) {
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        kMinimalManifest,
        R"("Type": "CBuffer", "Stages": ["Compute"])",
        R"("Type": "Texture", "Stages": ["Compute"], "Residency": "RootDescriptor")"));
    EXPECT_NE(diag.Message.find("RootDescriptor"), string::npos);
}

TEST(ShaderAssetTest, RejectsRootDescriptorArray) {
    ParseFail(Mutate(
        kMinimalManifest,
        R"("Type": "CBuffer", "Stages": ["Compute"])",
        R"("Type": "CBuffer", "Count": 4, "Stages": ["Compute"], "Residency": "RootDescriptor")"));
}

TEST(ShaderAssetTest, RejectsImmutableSamplerOnNonSampler) {
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        kMinimalManifest,
        R"("Type": "CBuffer", "Stages": ["Compute"])",
        R"("Type": "CBuffer", "Stages": ["Compute"], "ImmutableSampler": {
             "AddressS":"Repeat","AddressT":"Repeat","AddressR":"Repeat",
             "MinFilter":"Linear","MagFilter":"Linear","MipmapFilter":"Linear"}
        )"));
    EXPECT_NE(diag.Message.find("Sampler"), string::npos);
}

TEST(ShaderAssetTest, RejectsImmutableSamplerArray) {
    ParseFail(Mutate(
        kMinimalManifest,
        R"("Type": "CBuffer", "Stages": ["Compute"])",
        R"("Type": "Sampler", "Count": 2, "Stages": ["Compute"], "ImmutableSampler": {
             "AddressS":"Repeat","AddressT":"Repeat","AddressR":"Repeat",
             "MinFilter":"Linear","MagFilter":"Linear","MipmapFilter":"Linear"}
        )"));
}

// 这是本设计最有价值的静态检查之一: D3D12 里 b0 与 t0 互不冲突, Vulkan 里同一 set
// 内 binding 号必须全局唯一。不检查的话就会出现"DX 能跑、VK 炸掉"的 bug。
TEST(ShaderAssetTest, RejectsBindingNumberReuseAcrossRegisterClasses) {
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        kMinimalManifest,
        R"({ "Name": "gInput", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] })",
        R"({ "Name": "gInput", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] },
            { "Name": "gTex",   "Binding": 0, "Type": "Texture", "Stages": ["Compute"] })"));
    EXPECT_NE(diag.Message.find("Vulkan"), string::npos);
    EXPECT_EQ(diag.Group.value_or(999u), 0u);
}

TEST(ShaderAssetTest, RejectsBindingArrayRangeOverlap) {
    ParseFail(Mutate(
        kMinimalManifest,
        R"({ "Name": "gInput", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] })",
        R"({ "Name": "gArray", "Binding": 0, "Count": 4, "Type": "Texture", "Stages": ["Compute"] },
            { "Name": "gTex",   "Binding": 2, "Type": "Texture", "Stages": ["Compute"] })"));
}

TEST(ShaderAssetTest, AcceptsAdjacentBindingArrays) {
    const ShaderAssetDesc desc = ParseOk(Mutate(
        kMinimalManifest,
        R"({ "Name": "gInput", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] })",
        R"({ "Name": "gArray", "Binding": 0, "Count": 4, "Type": "Texture", "Stages": ["Compute"] },
            { "Name": "gTex",   "Binding": 4, "Type": "Texture", "Stages": ["Compute"] })"));
    // 数组绑定占据 [Binding, Binding + Count) 整段, 中间位置也应能查到。
    Nullable<const ShaderBindingDesc*> mid = desc.Passes.front().FindBinding(0, 2);
    ASSERT_TRUE(mid.HasValue());
    EXPECT_EQ(mid.Unwrap()->Name, "gArray");
    Nullable<const ShaderBindingDesc*> next = desc.Passes.front().FindBinding(0, 4);
    ASSERT_TRUE(next.HasValue());
    EXPECT_EQ(next.Unwrap()->Name, "gTex");
}

TEST(ShaderAssetTest, RejectsDuplicateBindingNameInGroup) {
    ParseFail(Mutate(
        kMinimalManifest,
        R"({ "Name": "gInput", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] })",
        R"({ "Name": "gInput", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] },
            { "Name": "gInput", "Binding": 1, "Type": "Texture", "Stages": ["Compute"] })"));
}

TEST(ShaderAssetTest, RejectsDuplicateBindingNameAcrossGroups) {
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        kMinimalManifest,
        R"("BindingGroups": [
        {
          "Group": 0,
          "Bindings": [
            { "Name": "gInput", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] }
          ]
        }
      ])",
        R"("BindingGroups": [
        { "Group": 0, "Bindings": [
            { "Name": "gInput", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] } ] },
        { "Group": 1, "Bindings": [
            { "Name": "gInput", "Binding": 0, "Type": "Texture", "Stages": ["Compute"] } ] }
      ])"));
    EXPECT_NE(diag.Message.find("group"), string::npos);
}

TEST(ShaderAssetTest, RejectsDuplicateGroupIndex) {
    ParseFail(Mutate(
        kMinimalManifest,
        R"("BindingGroups": [
        {
          "Group": 0,
          "Bindings": [
            { "Name": "gInput", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] }
          ]
        }
      ])",
        R"("BindingGroups": [
        { "Group": 0, "Bindings": [
            { "Name": "a", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] } ] },
        { "Group": 0, "Bindings": [
            { "Name": "b", "Binding": 1, "Type": "Texture", "Stages": ["Compute"] } ] }
      ])"));
}

TEST(ShaderAssetTest, RejectsEmptyBindingGroup) {
    ParseFail(Mutate(
        kMinimalManifest,
        R"("Bindings": [
            { "Name": "gInput", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] }
          ])",
        R"("Bindings": [])"));
}

TEST(ShaderAssetTest, RejectsExceedingRootDwordBudget) {
    // 33 个 root descriptor = 66 DWORD > 64。
    std::string bindings;
    for (uint32_t i = 0; i < 33; ++i) {
        if (i != 0) {
            bindings += ",";
        }
        bindings += "{ \"Name\": \"b" + std::to_string(i) +
                    "\", \"Binding\": " + std::to_string(i) +
                    ", \"Type\": \"CBuffer\", \"Stages\": [\"Compute\"],"
                    " \"Residency\": \"RootDescriptor\" }";
    }
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        kMinimalManifest,
        R"({ "Name": "gInput", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] })",
        bindings));
    EXPECT_NE(diag.Message.find("DWORD"), string::npos);
}

// ==================== 负例: push constant ====================

TEST(ShaderAssetTest, RejectsUnalignedPushConstantSize) {
    const ShaderAssetDiagnostic diag =
        ParseFail(Mutate(kImGuiManifest, "\"Size\": 16", "\"Size\": 14"));
    EXPECT_NE(diag.Message.find("aligned"), string::npos);
}

TEST(ShaderAssetTest, RejectsZeroPushConstantSize) {
    ParseFail(Mutate(kImGuiManifest, "\"Size\": 16", "\"Size\": 0"));
}

TEST(ShaderAssetTest, RejectsPushConstantCollidingWithBinding) {
    // 把 push constant 挪到 group 1 / binding 0, 与 gTexture 撞位。
    const ShaderAssetDiagnostic diag = ParseFail(
        Mutate(kImGuiManifest, R"("Location": { "Group": 0, "Binding": 0 })",
               R"("Location": { "Group": 1, "Binding": 0 })"));
    EXPECT_NE(diag.Message.find("collides"), string::npos);
}

TEST(ShaderAssetTest, RejectsPushConstantStageNotDeclaredByPass) {
    ParseFail(Mutate(
        kMinimalManifest,
        R"("BindingGroups": [)",
        R"("PushConstant": { "Name": "gPush",
              "Location": { "Group": 9, "Binding": 0 },
              "Size": 16, "Stages": ["Vertex"] },
           "BindingGroups": [)"));
}

// ==================== 负例: vertex input ====================

TEST(ShaderAssetTest, RejectsVertexAttributeExceedingStride) {
    const ShaderAssetDiagnostic diag = ParseFail(
        Mutate(kImGuiManifest, R"("ArrayStride": 20)", R"("ArrayStride": 16)"));
    EXPECT_NE(diag.Message.find("ArrayStride"), string::npos);
}

TEST(ShaderAssetTest, RejectsVertexAttributeOnUndeclaredBuffer) {
    ParseFail(Mutate(
        kImGuiManifest,
        R"({ "Semantic": "COLOR",    "SemanticIndex": 0, "Format": "UNORM8X4",  "BufferBinding": 0, "Offset": 16 })",
        R"({ "Semantic": "COLOR",    "SemanticIndex": 0, "Format": "UNORM8X4",  "BufferBinding": 3, "Offset": 0 })"));
}

TEST(ShaderAssetTest, RejectsDuplicateVertexSemantic) {
    ParseFail(Mutate(
        kImGuiManifest,
        R"({ "Semantic": "TEXCOORD", "SemanticIndex": 0, "Format": "FLOAT32X2", "BufferBinding": 0, "Offset": 8 })",
        R"({ "Semantic": "POSITION", "SemanticIndex": 0, "Format": "FLOAT32X2", "BufferBinding": 0, "Offset": 8 })"));
}

TEST(ShaderAssetTest, RejectsDuplicateVertexLocation) {
    ParseFail(Mutate(
        kImGuiManifest,
        R"({ "Semantic": "TEXCOORD", "SemanticIndex": 0, "Format": "FLOAT32X2", "BufferBinding": 0, "Offset": 8 })",
        R"({ "Semantic": "TEXCOORD", "SemanticIndex": 0, "Format": "FLOAT32X2", "BufferBinding": 0, "Offset": 8, "Location": 0 })"));
}

TEST(ShaderAssetTest, RejectsZeroArrayStride) {
    ParseFail(Mutate(kImGuiManifest, R"("ArrayStride": 20)", R"("ArrayStride": 0)"));
}

// ==================== 负例: keyword 组 ====================

TEST(ShaderAssetTest, RejectsDuplicateKeywordGroupName) {
    ParseFail(Mutate(
        kForwardManifest,
        R"({ "Name": "NormalMap",     "Keywords": ["_NORMAL_MAP"],          "IsOptional": true, "Stages": ["Pixel"] })",
        R"({ "Name": "BaseColorMap",  "Keywords": ["_NORMAL_MAP"],          "IsOptional": true, "Stages": ["Pixel"] })"));
}

TEST(ShaderAssetTest, RejectsKeywordSharedAcrossGroups) {
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        kForwardManifest,
        R"({ "Name": "NormalMap",     "Keywords": ["_NORMAL_MAP"],          "IsOptional": true, "Stages": ["Pixel"] })",
        R"({ "Name": "NormalMap",     "Keywords": ["_BASECOLOR_MAP"],       "IsOptional": true, "Stages": ["Pixel"] })"));
    EXPECT_NE(diag.Message.find("_BASECOLOR_MAP"), string::npos);
}

TEST(ShaderAssetTest, RejectsEmptyKeywordInGroup) {
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        kForwardManifest,
        R"("Keywords": ["_NORMAL_MAP"])",
        R"("Keywords": ["_NORMAL_MAP", ""])"));
    EXPECT_NE(diag.Message.find("IsOptional"), string::npos);
}

TEST(ShaderAssetTest, RejectsKeywordGroupWithoutKeywords) {
    ParseFail(Mutate(kForwardManifest, R"("Keywords": ["_NORMAL_MAP"])", R"("Keywords": [])"));
}

// ==================== pass 级 keyword 组引用 ====================

TEST(ShaderAssetTest, ParsesPassKeywordGroupSubset) {
    const ShaderAssetDesc desc = ParseOk(Mutate(
        kForwardManifest,
        R"("ShaderModel": "SM62",)",
        R"("ShaderModel": "SM62", "KeywordGroups": ["AlphaMode", "PointShadows"],)"));
    ASSERT_EQ(desc.Passes.size(), 1u);
    ASSERT_EQ(desc.Passes[0].KeywordGroups.size(), 2u);
    EXPECT_EQ(desc.Passes[0].KeywordGroups[0], "AlphaMode");
    EXPECT_EQ(desc.Passes[0].KeywordGroups[1], "PointShadows");
}

TEST(ShaderAssetTest, PassKeywordGroupsDefaultToEmptyMeaningAllGroups) {
    const ShaderAssetDesc desc = ParseOk(kForwardManifest);
    ASSERT_EQ(desc.Passes.size(), 1u);
    EXPECT_TRUE(desc.Passes[0].KeywordGroups.empty());
}

TEST(ShaderAssetTest, RejectsPassReferencingUnknownKeywordGroup) {
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        kForwardManifest,
        R"("ShaderModel": "SM62",)",
        R"("ShaderModel": "SM62", "KeywordGroups": ["NoSuchGroup"],)"));
    EXPECT_NE(diag.Message.find("NoSuchGroup"), string::npos);
    EXPECT_EQ(diag.PassName, "Forward");
}

TEST(ShaderAssetTest, RejectsPassListingKeywordGroupTwice) {
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        kForwardManifest,
        R"("ShaderModel": "SM62",)",
        R"("ShaderModel": "SM62", "KeywordGroups": ["AlphaMode", "AlphaMode"],)"));
    EXPECT_NE(diag.Message.find("AlphaMode"), string::npos);
}

/// kMinimalManifest (单个 Compute pass) 加上一个只作用于 Pixel 的资产级组。
std::string MinimalWithPixelOnlyGroup() {
    return Mutate(
        kMinimalManifest,
        R"(  "Passes": [)",
        R"(  "KeywordGroups": [
    { "Name": "PixelOnly", "Keywords": ["_PIXEL_ONLY"], "Stages": ["Pixel"] }
  ],
  "Passes": [)");
}

TEST(ShaderAssetTest, RejectsExplicitKeywordGroupWithNoStageOverlap) {
    // Compute pass 显式引用只作用于 Pixel 的组: 该组产生不了任何宏, 是哑配置。
    // 显式声明必须被严格核对, 故报错而非静默忽略。
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        MinimalWithPixelOnlyGroup(),
        R"("Stages": [{ "Stage": "Compute", "EntryPoint": "CSMain" }],)",
        R"("Stages": [{ "Stage": "Compute", "EntryPoint": "CSMain" }], "KeywordGroups": ["PixelOnly"],)"));
    EXPECT_NE(diag.Message.find("PixelOnly"), string::npos);
    EXPECT_EQ(diag.PassName, "Main");
}

TEST(ShaderAssetTest, AcceptsInheritedKeywordGroupWithNoStageOverlap) {
    // 同一个组, 但 pass 不显式引用 (继承全部)。共享的资产级默认值必须能被裁剪,
    // 否则每个 pass 都得复写一遍组列表。这里静默投影掉, 不报错。
    const ShaderAssetDesc desc = ParseOk(MinimalWithPixelOnlyGroup());
    ASSERT_EQ(desc.KeywordGroups.size(), 1u);
    ASSERT_EQ(desc.Passes.size(), 1u);
    EXPECT_TRUE(desc.Passes[0].KeywordGroups.empty());
}

TEST(ShaderAssetTest, RejectsPassDefineThatShadowsAKeyword) {
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        kForwardManifest,
        R"("ShaderModel": "SM62",)",
        R"("ShaderModel": "SM62", "Defines": ["_NORMAL_MAP"],)"));
    EXPECT_NE(diag.Message.find("_NORMAL_MAP"), string::npos);
    EXPECT_NE(diag.Message.find("NormalMap"), string::npos);
}

TEST(ShaderAssetTest, RejectsPassDefineThatShadowsAKeywordWithValue) {
    // FOO=1 形式也要按名字比对, 否则可以绕过上一条规则。
    ParseFail(Mutate(
        kForwardManifest,
        R"("ShaderModel": "SM62",)",
        R"("ShaderModel": "SM62", "Defines": ["_NORMAL_MAP=1"],)"));
}

TEST(ShaderAssetTest, AcceptsPassDefineThatIsNotAKeyword) {
    const ShaderAssetDesc desc = ParseOk(Mutate(
        kForwardManifest,
        R"("ShaderModel": "SM62",)",
        R"("ShaderModel": "SM62", "Defines": ["RADRAY_UNRELATED=1"],)"));
    ASSERT_EQ(desc.Passes.size(), 1u);
    ASSERT_EQ(desc.Passes[0].Defines.size(), 1u);
}

// ==================== 变体域 ====================
//
// 全部基于 manifest, 不需要 DXC 或反射。

/// 从 manifest 文本建 (第一个 pass 的) 变体域。
ShaderVariantDomain BuildDomain(std::string_view json) {
    const ShaderAssetDesc desc = ParseOk(json);
    EXPECT_FALSE(desc.Passes.empty());
    ShaderAssetDiagnostic diag{};
    std::optional<ShaderVariantDomain> domain =
        ShaderVariantDomain::Build(desc, desc.Passes.front(), diag);
    EXPECT_TRUE(domain.has_value()) << diag.ToString();
    return domain.has_value() ? std::move(domain.value()) : ShaderVariantDomain{};
}

ShaderVariantKey ResolveOk(
    const ShaderVariantDomain& domain,
    std::initializer_list<std::string_view> keywords) {
    const vector<std::string_view> list{keywords.begin(), keywords.end()};
    ShaderAssetDiagnostic diag{};
    std::optional<ShaderVariantKey> key = domain.Resolve(list, diag);
    EXPECT_TRUE(key.has_value()) << diag.ToString();
    return key.has_value() ? std::move(key.value()) : ShaderVariantKey{};
}

ShaderAssetDiagnostic ResolveFail(
    const ShaderVariantDomain& domain,
    std::initializer_list<std::string_view> keywords) {
    const vector<std::string_view> list{keywords.begin(), keywords.end()};
    ShaderAssetDiagnostic diag{};
    std::optional<ShaderVariantKey> key = domain.Resolve(list, diag);
    EXPECT_FALSE(key.has_value()) << "expected resolve failure but it succeeded";
    EXPECT_FALSE(diag.Message.empty());
    return diag;
}

TEST(ShaderVariantTest, KeyLengthMatchesGroupCount) {
    const ShaderVariantDomain domain = BuildDomain(kForwardManifest);
    EXPECT_EQ(domain.GroupCount(), 4u);
    const ShaderVariantKey key = domain.DefaultVariant();
    EXPECT_EQ(key.Selection.size(), 4u);
    EXPECT_TRUE(domain.IsValid(key));
}

TEST(ShaderVariantTest, DomainWithoutKeywordGroupsIsEmpty) {
    const ShaderVariantDomain domain = BuildDomain(kMinimalManifest);
    EXPECT_EQ(domain.GroupCount(), 0u);
    EXPECT_TRUE(domain.DefaultVariant().Selection.empty());
    EXPECT_TRUE(domain.IsValid(ShaderVariantKey{}));
}

TEST(ShaderVariantTest, PassKeywordGroupsRestrictTheDomain) {
    const ShaderVariantDomain domain = BuildDomain(Mutate(
        kForwardManifest,
        R"("ShaderModel": "SM62",)",
        R"("ShaderModel": "SM62", "KeywordGroups": ["AlphaMode"],)"));
    EXPECT_EQ(domain.GroupCount(), 1u);
    // 被裁掉的组的 keyword 不再属于本域。
    EXPECT_FALSE(domain.FindKeyword("_NORMAL_MAP").has_value());
    EXPECT_TRUE(domain.FindKeyword("_ALPHATEST_ON").has_value());
}

TEST(ShaderVariantTest, DefaultVariantIsAllOffWhenEveryGroupIsOptional) {
    const ShaderVariantDomain domain = BuildDomain(kForwardManifest);
    const ShaderVariantKey key = domain.DefaultVariant();
    for (const uint16_t selection : key.Selection) {
        EXPECT_EQ(selection, kShaderKeywordOff);
    }
    EXPECT_TRUE(domain.CollectDefines(key, render::ShaderStage::Pixel).empty());
}

TEST(ShaderVariantTest, DefaultVariantPicksFirstKeywordOfRequiredGroup) {
    const ShaderVariantDomain domain = BuildDomain(Mutate(
        kForwardManifest,
        R"({ "Name": "AlphaMode",     "Keywords": ["_ALPHATEST_ON", "_ALPHABLEND_ON"], "IsOptional": true, "Stages": ["Pixel"] })",
        R"({ "Name": "AlphaMode",     "Keywords": ["_ALPHATEST_ON", "_ALPHABLEND_ON"], "IsOptional": false, "Stages": ["Pixel"] })"));
    const ShaderVariantKey key = domain.DefaultVariant();
    const vector<string> defines = domain.CollectDefines(key, render::ShaderStage::Pixel);
    ASSERT_EQ(defines.size(), 1u);
    EXPECT_EQ(defines[0], "_ALPHATEST_ON");
}

TEST(ShaderVariantTest, ResolveSelectsRequestedKeywords) {
    const ShaderVariantDomain domain = BuildDomain(kForwardManifest);
    const ShaderVariantKey key = ResolveOk(domain, {"_NORMAL_MAP", "_ALPHABLEND_ON"});
    EXPECT_TRUE(domain.IsValid(key));
    const vector<string> names = domain.DescribeKeywords(key, render::ShaderStage::Pixel);
    ASSERT_EQ(names.size(), 2u);
    // DescribeKeywords 已排序。
    EXPECT_EQ(names[0], "_ALPHABLEND_ON");
    EXPECT_EQ(names[1], "_NORMAL_MAP");
}

TEST(ShaderVariantTest, ResolveRejectsUnknownKeyword) {
    const ShaderVariantDomain domain = BuildDomain(kForwardManifest);
    const ShaderAssetDiagnostic diag = ResolveFail(domain, {"_NO_SUCH_KEYWORD"});
    EXPECT_NE(diag.Message.find("_NO_SUCH_KEYWORD"), string::npos);
}

TEST(ShaderVariantTest, ResolveRejectsTwoKeywordsFromSameGroup) {
    const ShaderVariantDomain domain = BuildDomain(kForwardManifest);
    const ShaderAssetDiagnostic diag = ResolveFail(domain, {"_ALPHATEST_ON", "_ALPHABLEND_ON"});
    EXPECT_NE(diag.Message.find("AlphaMode"), string::npos);
}

TEST(ShaderVariantTest, ResolveAcceptsTheSameKeywordTwice) {
    // 重复请求同一个 keyword 不是冲突, 只是冗余。
    const ShaderVariantDomain domain = BuildDomain(kForwardManifest);
    const ShaderVariantKey key = ResolveOk(domain, {"_ALPHATEST_ON", "_ALPHATEST_ON"});
    EXPECT_EQ(domain.DescribeKeywords(key, render::ShaderStage::Pixel).size(), 1u);
}

TEST(ShaderVariantTest, ResolveRejectsMissingRequiredGroup) {
    const ShaderVariantDomain domain = BuildDomain(Mutate(
        kForwardManifest,
        R"({ "Name": "AlphaMode",     "Keywords": ["_ALPHATEST_ON", "_ALPHABLEND_ON"], "IsOptional": true, "Stages": ["Pixel"] })",
        R"({ "Name": "AlphaMode",     "Keywords": ["_ALPHATEST_ON", "_ALPHABLEND_ON"], "IsOptional": false, "Stages": ["Pixel"] })"));
    const ShaderAssetDiagnostic diag = ResolveFail(domain, {"_NORMAL_MAP"});
    EXPECT_NE(diag.Message.find("AlphaMode"), string::npos);
}

TEST(ShaderVariantTest, WithKeywordClearsSiblingsInSameGroup) {
    const ShaderVariantDomain domain = BuildDomain(kForwardManifest);
    const ShaderVariantKey alphaTest = ResolveOk(domain, {"_ALPHATEST_ON"});
    std::optional<ShaderVariantKey> alphaBlend =
        domain.WithKeyword(alphaTest, "_ALPHABLEND_ON", true);
    ASSERT_TRUE(alphaBlend.has_value());
    const vector<string> names = domain.DescribeKeywords(alphaBlend.value(), render::ShaderStage::Pixel);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "_ALPHABLEND_ON");
}

TEST(ShaderVariantTest, WithKeywordDisableIsNoOpWhenNotSelected) {
    const ShaderVariantDomain domain = BuildDomain(kForwardManifest);
    const ShaderVariantKey key = domain.DefaultVariant();
    std::optional<ShaderVariantKey> result = domain.WithKeyword(key, "_NORMAL_MAP", false);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), key);
}

TEST(ShaderVariantTest, WithKeywordRejectsUnknownKeyword) {
    const ShaderVariantDomain domain = BuildDomain(kForwardManifest);
    EXPECT_FALSE(domain.WithKeyword(domain.DefaultVariant(), "_NOPE", true).has_value());
}

TEST(ShaderVariantTest, WithKeywordRefusesToEmptyARequiredGroup) {
    const ShaderVariantDomain domain = BuildDomain(Mutate(
        kForwardManifest,
        R"({ "Name": "AlphaMode",     "Keywords": ["_ALPHATEST_ON", "_ALPHABLEND_ON"], "IsOptional": true, "Stages": ["Pixel"] })",
        R"({ "Name": "AlphaMode",     "Keywords": ["_ALPHATEST_ON", "_ALPHABLEND_ON"], "IsOptional": false, "Stages": ["Pixel"] })"));
    const ShaderVariantKey key = ResolveOk(domain, {"_ALPHATEST_ON"});
    EXPECT_FALSE(domain.WithKeyword(key, "_ALPHATEST_ON", false).has_value());
}

TEST(ShaderVariantTest, ProjectionDropsPixelOnlyKeywordsFromVertexStage) {
    const ShaderVariantDomain domain = BuildDomain(kForwardManifest);
    const ShaderVariantKey key = ResolveOk(domain, {"_NORMAL_MAP", "_POINT_SHADOWS"});

    const vector<string> pixel = domain.DescribeKeywords(key, render::ShaderStage::Pixel);
    ASSERT_EQ(pixel.size(), 2u);

    // _NORMAL_MAP 是 Pixel-only, _POINT_SHADOWS 覆盖 Vertex + Pixel。
    const vector<string> vertex = domain.DescribeKeywords(key, render::ShaderStage::Vertex);
    ASSERT_EQ(vertex.size(), 1u);
    EXPECT_EQ(vertex[0], "_POINT_SHADOWS");
}

TEST(ShaderVariantTest, ProjectionNormalizesUnrelatedGroupsToOff) {
    const ShaderVariantDomain domain = BuildDomain(kForwardManifest);
    const ShaderVariantKey key = ResolveOk(domain, {"_BASECOLOR_MAP", "_NORMAL_MAP", "_ALPHATEST_ON"});
    const ShaderVariantKey projected = domain.ProjectToStage(key, render::ShaderStage::Vertex);
    // 全部三个组都是 Pixel-only, 故 Vertex 投影后应当与"全关"完全相同。
    for (const uint16_t selection : projected.Selection) {
        EXPECT_EQ(selection, kShaderKeywordOff);
    }
    EXPECT_TRUE(domain.IsValid(projected));
    EXPECT_TRUE(domain.CollectDefines(projected, render::ShaderStage::Vertex).empty());
}

TEST(ShaderVariantTest, ProjectionOfRequiredGroupCollapsesWhenStageUnrelated) {
    // 必选组也必须归 Off: 否则两个只在 Pixel 上不同的变体会算出不同的 VS key,
    // stage 去重就失效了。IsValid 因此不能按 IsOptional 拒绝 Off。
    const ShaderVariantDomain domain = BuildDomain(Mutate(
        kForwardManifest,
        R"({ "Name": "AlphaMode",     "Keywords": ["_ALPHATEST_ON", "_ALPHABLEND_ON"], "IsOptional": true, "Stages": ["Pixel"] })",
        R"({ "Name": "AlphaMode",     "Keywords": ["_ALPHATEST_ON", "_ALPHABLEND_ON"], "IsOptional": false, "Stages": ["Pixel"] })"));
    const ShaderVariantKey test = ResolveOk(domain, {"_ALPHATEST_ON"});
    const ShaderVariantKey blend = ResolveOk(domain, {"_ALPHABLEND_ON"});
    EXPECT_NE(test, blend);

    const ShaderVariantKey testVs = domain.ProjectToStage(test, render::ShaderStage::Vertex);
    const ShaderVariantKey blendVs = domain.ProjectToStage(blend, render::ShaderStage::Vertex);
    EXPECT_EQ(testVs, blendVs);
    EXPECT_TRUE(domain.IsValid(testVs));
}

TEST(ShaderVariantTest, ProjectionIsIdempotent) {
    const ShaderVariantDomain domain = BuildDomain(kForwardManifest);
    const ShaderVariantKey key = ResolveOk(domain, {"_NORMAL_MAP", "_POINT_SHADOWS"});
    const ShaderVariantKey once = domain.ProjectToStage(key, render::ShaderStage::Vertex);
    const ShaderVariantKey twice = domain.ProjectToStage(once, render::ShaderStage::Vertex);
    EXPECT_EQ(once, twice);
}

TEST(ShaderVariantTest, CollectDefinesIncludesPassDefines) {
    const ShaderVariantDomain domain = BuildDomain(Mutate(
        kForwardManifest,
        R"("ShaderModel": "SM62",)",
        R"("ShaderModel": "SM62", "Defines": ["RADRAY_UNRELATED=1"],)"));
    const ShaderVariantKey key = ResolveOk(domain, {"_NORMAL_MAP"});
    const vector<string> defines = domain.CollectDefines(key, render::ShaderStage::Pixel);
    ASSERT_EQ(defines.size(), 2u);
    EXPECT_EQ(defines[0], "RADRAY_UNRELATED=1");
    EXPECT_EQ(defines[1], "_NORMAL_MAP");

    // pass.Defines 是无条件的, 投影不会把它去掉。
    const vector<string> vertex = domain.CollectDefines(key, render::ShaderStage::Vertex);
    ASSERT_EQ(vertex.size(), 1u);
    EXPECT_EQ(vertex[0], "RADRAY_UNRELATED=1");
}

TEST(ShaderVariantTest, InvalidKeyIsRejected) {
    const ShaderVariantDomain domain = BuildDomain(kForwardManifest);
    ShaderVariantKey shortKey;
    shortKey.Selection.assign(2, kShaderKeywordOff);
    EXPECT_FALSE(domain.IsValid(shortKey));

    ShaderVariantKey outOfRange = domain.DefaultVariant();
    outOfRange.Selection[0] = 7;  // BaseColorMap 只有一个 keyword
    EXPECT_FALSE(domain.IsValid(outOfRange));
}

TEST(ShaderVariantTest, ManyKeywordsBeyondSixtyFourWork) {
    // 变体身份按【组】编码, 故 keyword 总数不受 64 位之类的上限约束。
    // 这里造 80 个单 keyword 组, 全部覆盖 Compute。
    std::string groups;
    for (uint32_t i = 0; i < 80; ++i) {
        if (i != 0) {
            groups += ",\n    ";
        }
        groups += fmt::format(
            R"({{ "Name": "G{}", "Keywords": ["_KW_{}"], "Stages": ["Compute"] }})",
            i,
            i);
    }
    const std::string json = Mutate(
        kMinimalManifest,
        R"(  "Passes": [)",
        fmt::format("  \"KeywordGroups\": [\n    {}\n  ],\n  \"Passes\": [", groups));

    const ShaderVariantDomain domain = BuildDomain(json);
    ASSERT_EQ(domain.GroupCount(), 80u);

    // 全开。
    vector<std::string_view> all;
    vector<string> names;
    names.reserve(80);
    for (uint32_t i = 0; i < 80; ++i) {
        names.push_back(fmt::format("_KW_{}", i));
    }
    for (const string& name : names) {
        all.emplace_back(name);
    }
    ShaderAssetDiagnostic diag{};
    std::optional<ShaderVariantKey> key = domain.Resolve(all, diag);
    ASSERT_TRUE(key.has_value()) << diag.ToString();
    EXPECT_EQ(key->Selection.size(), 80u);
    EXPECT_EQ(domain.CollectDefines(key.value(), render::ShaderStage::Compute).size(), 80u);

    // 第 70 个 (远超 64) 能被单独关掉, 证明高位下标没有被截断。
    std::optional<ShaderVariantKey> without = domain.WithKeyword(key.value(), "_KW_70", false);
    ASSERT_TRUE(without.has_value());
    EXPECT_EQ(domain.CollectDefines(without.value(), render::ShaderStage::Compute).size(), 79u);
}

TEST(ShaderVariantTest, BuildRejectsUnknownGroupReference) {
    // ParseShaderAssetDesc 已拦下这种 manifest, 故这里手工构造 desc 绕过解析,
    // 确认 Build 自己也守住了前置条件。
    ShaderAssetDesc desc{};
    desc.Name = "Handmade";
    desc.Source = "a.hlsl";
    ShaderPassDesc pass{};
    pass.Name = "Main";
    pass.Stages.push_back(ShaderStageDesc{render::ShaderStage::Compute, "CSMain"});
    pass.KeywordGroups.push_back("Missing");
    desc.Passes.push_back(pass);

    ShaderAssetDiagnostic diag{};
    EXPECT_FALSE(ShaderVariantDomain::Build(desc, desc.Passes.front(), diag).has_value());
    EXPECT_NE(diag.Message.find("Missing"), string::npos);
}

// ==================== 烘焙集 ====================

/// 把一段 BakeVariants 声明插到 kForwardManifest 的资产级。
std::string ForwardWithAssetBake(std::string_view bakeJson) {
    return Mutate(
        kForwardManifest,
        R"(  "Passes": [)",
        fmt::format("  \"BakeVariants\": {},\n  \"Passes\": [", bakeJson));
}

/// 把一段 BakeVariants 声明插到 kForwardManifest 唯一 pass 上。
std::string ForwardWithPassBake(std::string_view bakeJson) {
    return Mutate(
        kForwardManifest,
        R"("ShaderModel": "SM62",)",
        fmt::format(R"("ShaderModel": "SM62", "BakeVariants": {},)", bakeJson));
}

vector<ShaderVariantKey> ExpandOk(std::string_view json) {
    const ShaderAssetDesc desc = ParseOk(json);
    EXPECT_FALSE(desc.Passes.empty());
    if (desc.Passes.empty()) {
        return {};
    }
    const ShaderPassDesc& pass = desc.Passes.front();
    ShaderAssetDiagnostic diag{};
    std::optional<ShaderVariantDomain> domain = ShaderVariantDomain::Build(desc, pass, diag);
    EXPECT_TRUE(domain.has_value()) << diag.ToString();
    if (!domain.has_value()) {
        return {};
    }
    const bool isInherited = pass.BakeVariants.IsEmpty();
    std::optional<vector<ShaderVariantKey>> variants = ExpandShaderBakeSet(
        domain.value(),
        GetEffectiveBakeSet(desc, pass),
        isInherited,
        diag);
    EXPECT_TRUE(variants.has_value()) << diag.ToString();
    return variants.has_value() ? std::move(variants.value()) : vector<ShaderVariantKey>{};
}

/// 把变体列表转成可读的 keyword 集合列表, 便于断言。
vector<vector<string>> DescribeAll(
    const ShaderVariantDomain& domain,
    std::span<const ShaderVariantKey> variants,
    render::ShaderStage stage) {
    vector<vector<string>> result;
    result.reserve(variants.size());
    for (const ShaderVariantKey& key : variants) {
        result.push_back(domain.DescribeKeywords(key, stage));
    }
    return result;
}

TEST(ShaderBakeSetTest, EmptyBakeSetYieldsDefaultVariantOnly) {
    // 未声明 BakeVariants 的 manifest 语义不变: 只烘默认变体。
    const vector<ShaderVariantKey> variants = ExpandOk(kForwardManifest);
    ASSERT_EQ(variants.size(), 1u);
    const ShaderVariantDomain domain = BuildDomain(kForwardManifest);
    EXPECT_EQ(variants[0], domain.DefaultVariant());
}

TEST(ShaderBakeSetTest, ExpandProducesCartesianProductOfListedGroups) {
    // BaseColorMap: {off, _BASECOLOR_MAP} = 2
    // AlphaMode:    {off, _ALPHATEST_ON, _ALPHABLEND_ON} = 3
    const vector<ShaderVariantKey> variants = ExpandOk(ForwardWithAssetBake(
        R"({ "Rules": [{ "Expand": ["BaseColorMap", "AlphaMode"] }] })"));
    EXPECT_EQ(variants.size(), 6u);
}

TEST(ShaderBakeSetTest, ExpandLeavesUnlistedGroupsAtDefault) {
    const std::string json = ForwardWithAssetBake(
        R"({ "Rules": [{ "Expand": ["AlphaMode"] }] })");
    const vector<ShaderVariantKey> variants = ExpandOk(json);
    ASSERT_EQ(variants.size(), 3u);
    const ShaderVariantDomain domain = BuildDomain(json);
    // 未列出的三个组恒为关, 故每个变体最多只有 AlphaMode 的一个 keyword。
    for (const ShaderVariantKey& key : variants) {
        EXPECT_LE(domain.DescribeKeywords(key, render::ShaderStage::Pixel).size(), 1u);
    }
}

TEST(ShaderBakeSetTest, ExpandOfRequiredGroupOmitsTheOffValue) {
    const std::string json = Mutate(
        ForwardWithAssetBake(R"({ "Rules": [{ "Expand": ["AlphaMode"] }] })"),
        R"({ "Name": "AlphaMode",     "Keywords": ["_ALPHATEST_ON", "_ALPHABLEND_ON"], "IsOptional": true, "Stages": ["Pixel"] })",
        R"({ "Name": "AlphaMode",     "Keywords": ["_ALPHATEST_ON", "_ALPHABLEND_ON"], "IsOptional": false, "Stages": ["Pixel"] })");
    const vector<ShaderVariantKey> variants = ExpandOk(json);
    // 必选组没有"关"这个取值, 故只有两个; 默认变体取首个 keyword, 已在其中。
    EXPECT_EQ(variants.size(), 2u);
}

TEST(ShaderBakeSetTest, ExplicitCombinationIsBaked) {
    const std::string json = ForwardWithAssetBake(
        R"({ "Rules": [{ "Combination": ["_POINT_SHADOWS", "_ALPHATEST_ON"] }] })");
    const vector<ShaderVariantKey> variants = ExpandOk(json);
    // 默认变体 + 这一个显式组合。
    ASSERT_EQ(variants.size(), 2u);
    const ShaderVariantDomain domain = BuildDomain(json);
    const vector<vector<string>> described =
        DescribeAll(domain, variants, render::ShaderStage::Pixel);
    EXPECT_TRUE(std::ranges::any_of(described, [](const vector<string>& names) {
        return names.size() == 2u;
    }));
}

TEST(ShaderBakeSetTest, OverlappingRulesAreDeduplicated) {
    // _ALPHATEST_ON 既在 Expand 的积里, 又被显式点名。重叠是正常写法, 不该报错,
    // 结果按变体去重。
    const vector<ShaderVariantKey> variants = ExpandOk(ForwardWithAssetBake(
        R"({ "Rules": [
          { "Expand": ["AlphaMode"] },
          { "Combination": ["_ALPHATEST_ON"] }
        ] })"));
    EXPECT_EQ(variants.size(), 3u);
}

TEST(ShaderBakeSetTest, ResultIsSortedAndUnique) {
    const vector<ShaderVariantKey> variants = ExpandOk(ForwardWithAssetBake(
        R"({ "Rules": [{ "Expand": ["BaseColorMap", "NormalMap", "AlphaMode"] }] })"));
    EXPECT_EQ(variants.size(), 12u);
    EXPECT_TRUE(std::ranges::is_sorted(variants));
    EXPECT_EQ(std::ranges::adjacent_find(variants), variants.end());
}

TEST(ShaderBakeSetTest, SkipRemovesCoOccurrence) {
    const std::string json = ForwardWithAssetBake(
        R"({
          "Rules": [{ "Expand": ["BaseColorMap", "AlphaMode"] }],
          "Skip": [["_BASECOLOR_MAP", "_ALPHABLEND_ON"]]
        })");
    const vector<ShaderVariantKey> variants = ExpandOk(json);
    // 6 个组合里去掉 (_BASECOLOR_MAP, _ALPHABLEND_ON) 这一个。
    EXPECT_EQ(variants.size(), 5u);
    const ShaderVariantDomain domain = BuildDomain(json);
    for (const vector<string>& names : DescribeAll(domain, variants, render::ShaderStage::Pixel)) {
        const bool both =
            std::ranges::find(names, "_BASECOLOR_MAP") != names.end() &&
            std::ranges::find(names, "_ALPHABLEND_ON") != names.end();
        EXPECT_FALSE(both);
    }
}

TEST(ShaderBakeSetTest, SkipDoesNotAffectExplicitCombination) {
    const std::string json = ForwardWithAssetBake(
        R"({
          "Rules": [
            { "Expand": ["BaseColorMap", "AlphaMode"] },
            { "Combination": ["_BASECOLOR_MAP", "_ALPHABLEND_ON"] }
          ],
          "Skip": [["_BASECOLOR_MAP", "_ALPHABLEND_ON"]]
        })");
    const vector<ShaderVariantKey> variants = ExpandOk(json);
    // Skip 把该组合从积里剔掉, 但显式点名的又补回来, 故仍是 6 个。
    EXPECT_EQ(variants.size(), 6u);
    const ShaderVariantDomain domain = BuildDomain(json);
    const vector<vector<string>> described =
        DescribeAll(domain, variants, render::ShaderStage::Pixel);
    EXPECT_TRUE(std::ranges::any_of(described, [](const vector<string>& names) {
        return std::ranges::find(names, "_BASECOLOR_MAP") != names.end() &&
               std::ranges::find(names, "_ALPHABLEND_ON") != names.end();
    }));
}

TEST(ShaderBakeSetTest, SkipNeverRemovesTheDefaultVariant) {
    // 造一条能命中默认变体的 Skip 是不可能的 (默认变体全关, Skip 至少两个
    // keyword), 但必选组的默认变体有选中的 keyword, 那就可能被命中。
    const std::string json = Mutate(
        ForwardWithAssetBake(
            R"({
              "Rules": [{ "Expand": ["AlphaMode", "PointShadows"] }],
              "Skip": [["_ALPHATEST_ON", "_POINT_SHADOWS"]]
            })"),
        R"({ "Name": "AlphaMode",     "Keywords": ["_ALPHATEST_ON", "_ALPHABLEND_ON"], "IsOptional": true, "Stages": ["Pixel"] })",
        R"({ "Name": "AlphaMode",     "Keywords": ["_ALPHATEST_ON", "_ALPHABLEND_ON"], "IsOptional": false, "Stages": ["Pixel"] })");
    const vector<ShaderVariantKey> variants = ExpandOk(json);
    const ShaderVariantDomain domain = BuildDomain(json);
    EXPECT_NE(std::ranges::find(variants, domain.DefaultVariant()), variants.end());
}

TEST(ShaderBakeSetTest, PassBakeSetOverridesAssetLevel) {
    const std::string json = Mutate(
        ForwardWithAssetBake(R"({ "Rules": [{ "Expand": ["BaseColorMap", "AlphaMode"] }] })"),
        R"("ShaderModel": "SM62",)",
        R"("ShaderModel": "SM62", "BakeVariants": { "Rules": [{ "Expand": ["AlphaMode"] }] },)");
    // pass 自己声明了, 资产级的 6 个组合不生效。
    EXPECT_EQ(ExpandOk(json).size(), 3u);
}

TEST(ShaderBakeSetTest, PassInheritsAssetBakeSet) {
    EXPECT_EQ(
        ExpandOk(ForwardWithAssetBake(R"({ "Rules": [{ "Expand": ["AlphaMode"] }] })")).size(),
        3u);
}

TEST(ShaderBakeSetTest, InheritedRuleProjectsAwayGroupNotInPass) {
    // 资产级展开两个组, 但 pass 只包含其中一个。继承的规则静默投影掉另一个维度,
    // 而不是报错 —— 共享的资产级默认值必须能被裁剪。
    const std::string json = Mutate(
        ForwardWithAssetBake(R"({ "Rules": [{ "Expand": ["BaseColorMap", "AlphaMode"] }] })"),
        R"("ShaderModel": "SM62",)",
        R"("ShaderModel": "SM62", "KeywordGroups": ["AlphaMode"],)");
    const vector<ShaderVariantKey> variants = ExpandOk(json);
    // 只剩 AlphaMode 一个维度。
    EXPECT_EQ(variants.size(), 3u);
    const ShaderVariantDomain domain = BuildDomain(json);
    EXPECT_EQ(domain.GroupCount(), 1u);
}

TEST(ShaderBakeSetTest, InheritedCombinationIsSkippedWhenKeywordNotInPass) {
    // 同理, 继承的显式组合引用了被裁掉的 keyword 时整条跳过。
    const std::string json = Mutate(
        ForwardWithAssetBake(
            R"({ "Rules": [
              { "Combination": ["_BASECOLOR_MAP"] },
              { "Combination": ["_ALPHATEST_ON"] }
            ] })"),
        R"("ShaderModel": "SM62",)",
        R"("ShaderModel": "SM62", "KeywordGroups": ["AlphaMode"],)");
    const vector<ShaderVariantKey> variants = ExpandOk(json);
    // 默认变体 + _ALPHATEST_ON。_BASECOLOR_MAP 那条被跳过。
    EXPECT_EQ(variants.size(), 2u);
}

TEST(ShaderBakeSetTest, ExplicitPassRuleRejectsGroupNotInPass) {
    // 与继承相反: pass 显式写的规则引用本 pass 没有的组是错误。
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        kForwardManifest,
        R"("ShaderModel": "SM62",)",
        R"("ShaderModel": "SM62",
         "KeywordGroups": ["AlphaMode"],
         "BakeVariants": { "Rules": [{ "Expand": ["BaseColorMap"] }] },)"));
    EXPECT_NE(diag.Message.find("BaseColorMap"), string::npos);
}

// ---- 烘焙声明的负例 ----

TEST(ShaderBakeSetTest, RejectsRuleWithNeitherExpandNorCombination) {
    ParseFail(ForwardWithAssetBake(R"({ "Rules": [{}] })"));
}

TEST(ShaderBakeSetTest, RejectsRuleWithBothExpandAndCombination) {
    ParseFail(ForwardWithAssetBake(
        R"({ "Rules": [{ "Expand": ["AlphaMode"], "Combination": ["_NORMAL_MAP"] }] })"));
}

TEST(ShaderBakeSetTest, RejectsExpandOfUnknownGroup) {
    const ShaderAssetDiagnostic diag = ParseFail(ForwardWithAssetBake(
        R"({ "Rules": [{ "Expand": ["NoSuchGroup"] }] })"));
    EXPECT_NE(diag.Message.find("NoSuchGroup"), string::npos);
}

TEST(ShaderBakeSetTest, RejectsDuplicateGroupInExpand) {
    ParseFail(ForwardWithAssetBake(
        R"({ "Rules": [{ "Expand": ["AlphaMode", "AlphaMode"] }] })"));
}

TEST(ShaderBakeSetTest, RejectsCombinationWithUnknownKeyword) {
    const ShaderAssetDiagnostic diag = ParseFail(ForwardWithAssetBake(
        R"({ "Rules": [{ "Combination": ["_NOT_A_KEYWORD"] }] })"));
    EXPECT_NE(diag.Message.find("_NOT_A_KEYWORD"), string::npos);
}

TEST(ShaderBakeSetTest, RejectsCombinationWithTwoKeywordsFromSameGroup) {
    const ShaderAssetDiagnostic diag = ParseFail(ForwardWithAssetBake(
        R"({ "Rules": [{ "Combination": ["_ALPHATEST_ON", "_ALPHABLEND_ON"] }] })"));
    EXPECT_NE(diag.Message.find("AlphaMode"), string::npos);
}

TEST(ShaderBakeSetTest, RejectsDuplicateKeywordInCombination) {
    ParseFail(ForwardWithAssetBake(
        R"({ "Rules": [{ "Combination": ["_NORMAL_MAP", "_NORMAL_MAP"] }] })"));
}

TEST(ShaderBakeSetTest, RejectsSkipWithSingleKeyword) {
    const ShaderAssetDiagnostic diag = ParseFail(ForwardWithAssetBake(
        R"({ "Rules": [{ "Expand": ["AlphaMode"] }], "Skip": [["_ALPHATEST_ON"]] })"));
    EXPECT_NE(diag.Message.find("Expand"), string::npos);
}

TEST(ShaderBakeSetTest, RejectsSkipWithUnknownKeyword) {
    ParseFail(ForwardWithAssetBake(
        R"({ "Rules": [{ "Expand": ["AlphaMode"] }], "Skip": [["_ALPHATEST_ON", "_NOPE"]] })"));
}

TEST(ShaderBakeSetTest, RoundTripsThroughSerialization) {
    const std::string json = ForwardWithPassBake(
        R"({
          "Rules": [
            { "Expand": ["BaseColorMap", "AlphaMode"] },
            { "Combination": ["_POINT_SHADOWS"] }
          ],
          "Skip": [["_BASECOLOR_MAP", "_ALPHABLEND_ON"]]
        })");
    const ShaderAssetDesc desc = ParseOk(json);
    std::optional<string> text = SerializeShaderAssetDesc(desc);
    ASSERT_TRUE(text.has_value());
    const ShaderAssetDesc again = ParseOk(text.value());
    EXPECT_EQ(desc, again);

    ASSERT_EQ(again.Passes.size(), 1u);
    const ShaderBakeSetDesc& bake = again.Passes[0].BakeVariants;
    ASSERT_EQ(bake.Rules.size(), 2u);
    EXPECT_EQ(bake.Rules[0].Expand.size(), 2u);
    EXPECT_TRUE(bake.Rules[0].Combination.empty());
    EXPECT_TRUE(bake.Rules[1].Expand.empty());
    ASSERT_EQ(bake.Rules[1].Combination.size(), 1u);
    ASSERT_EQ(bake.Skip.size(), 1u);
    EXPECT_EQ(bake.Skip[0].size(), 2u);
}

TEST(ShaderBakeSetTest, AssetLevelBakeSetRoundTrips) {
    const std::string json = ForwardWithAssetBake(
        R"({ "Rules": [{ "Expand": ["AlphaMode"] }] })");
    const ShaderAssetDesc desc = ParseOk(json);
    std::optional<string> text = SerializeShaderAssetDesc(desc);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(desc, ParseOk(text.value()));
    EXPECT_FALSE(desc.BakeVariants.IsEmpty());
}

// ==================== 反射一致性校验 ====================
//
// 反射结构手工构造, 不依赖 DXC / SPIRV-Cross 是否启用, 因此测试是自洽的。
// 校验方向为【声明 ⊇ 反射】: 反射有而 manifest 无是错误; manifest 有而反射无 (DCE /
// keyword #ifdef) 是正常的。

render::HlslInputBindDesc MakeHlslBind(
    std::string_view name,
    render::HlslShaderInputType type,
    uint32_t space,
    uint32_t bindPoint,
    uint32_t bindCount = 1,
    render::HlslSRVDimension dim = render::HlslSRVDimension::TEXTURE2D) {
    render::HlslInputBindDesc bind{};
    bind.Name = string{name};
    bind.Type = type;
    bind.Space = space;
    bind.BindPoint = bindPoint;
    bind.BindCount = bindCount;
    bind.Dimension = dim;
    return bind;
}

render::SpirvResourceBinding MakeSpirvBind(
    std::string_view name,
    render::SpirvResourceKind kind,
    uint32_t set,
    uint32_t binding,
    uint32_t arraySize = 1) {
    render::SpirvResourceBinding bind{};
    bind.Name = string{name};
    bind.Kind = kind;
    bind.Set = set;
    bind.Binding = binding;
    bind.ArraySize = arraySize;
    if (kind == render::SpirvResourceKind::SeparateImage ||
        kind == render::SpirvResourceKind::SampledImage ||
        kind == render::SpirvResourceKind::StorageImage) {
        render::SpirvImageInfo info{};
        info.Dim = render::SpirvImageDim::Dim2D;
        bind.ImageInfo = info;
    }
    return bind;
}

/// 与 kImGuiManifest 相符的 DXIL PS 反射。
/// 注意 push constant 在 DXIL 里就是一个普通 cbuffer (b0, space0)。
render::HlslShaderDesc MakeImGuiPixelHlslReflection() {
    render::HlslShaderDesc desc{};
    desc.BoundResources.push_back(MakeHlslBind(
        "gTexture", render::HlslShaderInputType::TEXTURE, 1, 0));
    desc.BoundResources.push_back(MakeHlslBind(
        "gSampler", render::HlslShaderInputType::SAMPLER, 1, 1, 1,
        render::HlslSRVDimension::UNKNOWN));
    return desc;
}

render::HlslShaderDesc MakeImGuiVertexHlslReflection() {
    render::HlslShaderDesc desc{};
    desc.BoundResources.push_back(MakeHlslBind(
        "gPush", render::HlslShaderInputType::CBUFFER, 0, 0, 1,
        render::HlslSRVDimension::UNKNOWN));

    render::HlslShaderBufferDesc cbuffer{};
    cbuffer.Name = "gPush";
    cbuffer.Type = render::HlslCBufferType::CBUFFER;
    cbuffer.Size = 16;
    desc.ConstantBuffers.push_back(std::move(cbuffer));

    for (const char* semantic : {"POSITION", "TEXCOORD", "COLOR"}) {
        render::HlslSignatureParameterDesc input{};
        input.SemanticName = semantic;
        input.SemanticIndex = 0;
        desc.InputParameters.push_back(std::move(input));
    }
    return desc;
}

TEST(ShaderAssetTest, AcceptsMatchingHlslReflection) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    const ShaderPassDesc& pass = desc.Passes.front();

    ShaderAssetDiagnostic diag{};
    EXPECT_TRUE(ValidateShaderReflection(
        pass, render::ShaderStage::Pixel, MakeImGuiPixelHlslReflection(), diag))
        << diag.ToString();
    EXPECT_TRUE(ValidateShaderReflection(
        pass, render::ShaderStage::Vertex, MakeImGuiVertexHlslReflection(), diag))
        << diag.ToString();
}

TEST(ShaderAssetTest, AcceptsMatchingSpirvReflection) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    const ShaderPassDesc& pass = desc.Passes.front();

    render::SpirvShaderDesc ps{};
    ps.ResourceBindings.push_back(
        MakeSpirvBind("gTexture", render::SpirvResourceKind::SeparateImage, 1, 0));
    ps.ResourceBindings.push_back(
        MakeSpirvBind("gSampler", render::SpirvResourceKind::SeparateSampler, 1, 1));

    ShaderAssetDiagnostic diag{};
    EXPECT_TRUE(ValidateShaderReflection(pass, render::ShaderStage::Pixel, ps, diag))
        << diag.ToString();

    render::SpirvShaderDesc vs{};
    render::SpirvPushConstantRange range{};
    range.Name = "gPush";
    range.Offset = 0;
    range.Size = 16;
    vs.ConstantRanges.push_back(std::move(range));
    for (uint32_t location = 0; location < 3; ++location) {
        render::SpirvStageIo input{};
        input.Location = location;
        vs.StageInputs.push_back(std::move(input));
    }
    EXPECT_TRUE(ValidateShaderReflection(pass, render::ShaderStage::Vertex, vs, diag))
        << diag.ToString();
}

// 核心属性: 声明式 ABI 使被 keyword #ifdef 消掉的绑定不影响校验,
// 所有变体因此共用同一个 PipelineLayout。
TEST(ShaderAssetTest, AcceptsReflectionMissingDeclaredBindings) {
    const ShaderAssetDesc desc = ParseOk(kForwardManifest);
    const ShaderPassDesc& pass = desc.Passes.front();

    // 关掉阴影的变体: gShadowCube / gShadowArray / gShadowSampler 都不在反射里。
    render::HlslShaderDesc reflection{};
    reflection.BoundResources.push_back(MakeHlslBind(
        "gView", render::HlslShaderInputType::CBUFFER, 1, 0, 1,
        render::HlslSRVDimension::UNKNOWN));
    reflection.BoundResources.push_back(MakeHlslBind(
        "gMaterial", render::HlslShaderInputType::CBUFFER, 2, 0, 1,
        render::HlslSRVDimension::UNKNOWN));

    ShaderAssetDiagnostic diag{};
    EXPECT_TRUE(ValidateShaderReflection(pass, render::ShaderStage::Pixel, reflection, diag))
        << diag.ToString();
}

TEST(ShaderAssetTest, RejectsReflectionWithUndeclaredBinding) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    render::HlslShaderDesc reflection = MakeImGuiPixelHlslReflection();
    reflection.BoundResources.push_back(MakeHlslBind(
        "gExtra", render::HlslShaderInputType::TEXTURE, 1, 7));

    ShaderAssetDiagnostic diag{};
    EXPECT_FALSE(ValidateShaderReflection(
        desc.Passes.front(), render::ShaderStage::Pixel, reflection, diag));
    EXPECT_NE(diag.Message.find("gExtra"), string::npos);
    EXPECT_EQ(diag.Group.value_or(999u), 1u);
    EXPECT_EQ(diag.Binding.value_or(999u), 7u);
}

TEST(ShaderAssetTest, RejectsReflectionBindingTypeMismatch) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    render::HlslShaderDesc reflection{};
    // manifest 在 (1, 0) 声明的是 Texture, 这里反射说是 cbuffer。
    reflection.BoundResources.push_back(MakeHlslBind(
        "gTexture", render::HlslShaderInputType::CBUFFER, 1, 0, 1,
        render::HlslSRVDimension::UNKNOWN));

    ShaderAssetDiagnostic diag{};
    EXPECT_FALSE(ValidateShaderReflection(
        desc.Passes.front(), render::ShaderStage::Pixel, reflection, diag));
    EXPECT_NE(diag.Message.find("type mismatch"), string::npos);
}

TEST(ShaderAssetTest, RejectsReflectionBindingNameMismatch) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    render::HlslShaderDesc reflection{};
    reflection.BoundResources.push_back(MakeHlslBind(
        "gAlbedo", render::HlslShaderInputType::TEXTURE, 1, 0));

    ShaderAssetDiagnostic diag{};
    EXPECT_FALSE(ValidateShaderReflection(
        desc.Passes.front(), render::ShaderStage::Pixel, reflection, diag));
    EXPECT_NE(diag.Message.find("name mismatch"), string::npos);
}

TEST(ShaderAssetTest, RejectsReflectionUsingBindingInUndeclaredStage) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    // gTexture 只声明给 Pixel, 但反射说 VS 也用了它。
    render::HlslShaderDesc reflection{};
    reflection.BoundResources.push_back(MakeHlslBind(
        "gTexture", render::HlslShaderInputType::TEXTURE, 1, 0));

    ShaderAssetDiagnostic diag{};
    EXPECT_FALSE(ValidateShaderReflection(
        desc.Passes.front(), render::ShaderStage::Vertex, reflection, diag));
    EXPECT_NE(diag.Message.find("stage"), string::npos);
}

TEST(ShaderAssetTest, RejectsPushConstantSizeMismatchInHlslReflection) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    render::HlslShaderDesc reflection = MakeImGuiVertexHlslReflection();
    reflection.ConstantBuffers.front().Size = 32;

    ShaderAssetDiagnostic diag{};
    EXPECT_FALSE(ValidateShaderReflection(
        desc.Passes.front(), render::ShaderStage::Vertex, reflection, diag));
    EXPECT_NE(diag.Message.find("push constant size mismatch"), string::npos);
}

TEST(ShaderAssetTest, AcceptsHlslPushConstantPaddedToSixteenBytes) {
    // D3D 反射的 cbuffer Size 按 16 字节向上对齐, manifest 只要求 4 字节对齐。
    // 声明 4 字节时反射会报 16, 这不是不一致。
    ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    ASSERT_TRUE(desc.Passes.front().PushConstant.has_value());
    desc.Passes.front().PushConstant->Size = 4;

    render::HlslShaderDesc reflection = MakeImGuiVertexHlslReflection();
    reflection.ConstantBuffers.front().Size = 16;

    ShaderAssetDiagnostic diag{};
    EXPECT_TRUE(ValidateShaderReflection(
        desc.Passes.front(), render::ShaderStage::Vertex, reflection, diag))
        << diag.ToString();
}

TEST(ShaderAssetTest, RejectsSpirvPushConstantRangeExceedingDeclaredSize) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    render::SpirvShaderDesc vs{};
    render::SpirvPushConstantRange range{};
    range.Name = "gPush";
    range.Offset = 0;
    range.Size = 64;  // manifest 只声明 16 字节。
    vs.ConstantRanges.push_back(std::move(range));

    ShaderAssetDiagnostic diag{};
    EXPECT_FALSE(ValidateShaderReflection(
        desc.Passes.front(), render::ShaderStage::Vertex, vs, diag));
    EXPECT_NE(diag.Message.find("exceeds"), string::npos);
}

TEST(ShaderAssetTest, RejectsSpirvPushConstantWhenManifestDeclaresNone) {
    const ShaderAssetDesc desc = ParseOk(kForwardManifest);
    render::SpirvShaderDesc vs{};
    render::SpirvPushConstantRange range{};
    range.Name = "gStray";
    range.Size = 16;
    vs.ConstantRanges.push_back(std::move(range));

    ShaderAssetDiagnostic diag{};
    EXPECT_FALSE(ValidateShaderReflection(
        desc.Passes.front(), render::ShaderStage::Vertex, vs, diag));
    EXPECT_NE(diag.Message.find("push constant"), string::npos);
}

TEST(ShaderAssetTest, RejectsReflectionForStageNotDeclaredByPass) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    ShaderAssetDiagnostic diag{};
    EXPECT_FALSE(ValidateShaderReflection(
        desc.Passes.front(), render::ShaderStage::Compute, render::HlslShaderDesc{}, diag));
    EXPECT_NE(diag.Message.find("Compute"), string::npos);
}

TEST(ShaderAssetTest, RejectsUndeclaredVertexSemanticFromHlslReflection) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    render::HlslShaderDesc reflection = MakeImGuiVertexHlslReflection();
    render::HlslSignatureParameterDesc extra{};
    extra.SemanticName = "NORMAL";
    reflection.InputParameters.push_back(std::move(extra));

    ShaderAssetDiagnostic diag{};
    EXPECT_FALSE(ValidateShaderReflection(
        desc.Passes.front(), render::ShaderStage::Vertex, reflection, diag));
    EXPECT_NE(diag.Message.find("NORMAL"), string::npos);
}

// SV_ 系统值语义不由 vertex buffer 提供, 不应要求 manifest 声明。
TEST(ShaderAssetTest, IgnoresSystemValueSemanticsInVertexReflection) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    render::HlslShaderDesc reflection = MakeImGuiVertexHlslReflection();
    render::HlslSignatureParameterDesc sv{};
    sv.SemanticName = "SV_InstanceID";
    sv.SystemValueType = render::HlslSystemValueType::INSTANCE_ID;
    reflection.InputParameters.push_back(std::move(sv));

    ShaderAssetDiagnostic diag{};
    EXPECT_TRUE(ValidateShaderReflection(
        desc.Passes.front(), render::ShaderStage::Vertex, reflection, diag))
        << diag.ToString();
}

// SPIRV 的 builtin 输入同理不参与比对。
TEST(ShaderAssetTest, IgnoresBuiltInInputsInSpirvVertexReflection) {
    const ShaderAssetDesc desc = ParseOk(kImGuiManifest);
    render::SpirvShaderDesc vs{};
    for (uint32_t location = 0; location < 3; ++location) {
        render::SpirvStageIo input{};
        input.Location = location;
        vs.StageInputs.push_back(std::move(input));
    }
    render::SpirvStageIo builtIn{};
    builtIn.Location = 42;
    builtIn.BuiltIn = 5;
    vs.StageInputs.push_back(std::move(builtIn));

    ShaderAssetDiagnostic diag{};
    EXPECT_TRUE(ValidateShaderReflection(
        desc.Passes.front(), render::ShaderStage::Vertex, vs, diag))
        << diag.ToString();
}

// unbounded 数组: 反射只说"unbounded", manifest 的容量是权威值, 不应报 count 不符。
TEST(ShaderAssetTest, AcceptsUnboundedReflectionArrayAgainstDeclaredCapacity) {
    const ShaderAssetDesc desc = ParseOk(Mutate(
        kMinimalManifest,
        R"({ "Name": "gInput", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] })",
        R"({ "Name": "gTextures", "Binding": 0, "Count": 64, "Type": "Texture", "Stages": ["Compute"] })"));

    render::HlslShaderDesc reflection{};
    reflection.BoundResources.push_back(MakeHlslBind(
        "gTextures", render::HlslShaderInputType::TEXTURE, 0, 0, 0));
    ASSERT_TRUE(reflection.BoundResources.front().IsUnboundArray());

    ShaderAssetDiagnostic diag{};
    EXPECT_TRUE(ValidateShaderReflection(
        desc.Passes.front(), render::ShaderStage::Compute, reflection, diag))
        << diag.ToString();
}

TEST(ShaderAssetTest, RejectsBoundedReflectionArrayCountMismatch) {
    const ShaderAssetDesc desc = ParseOk(Mutate(
        kMinimalManifest,
        R"({ "Name": "gInput", "Binding": 0, "Type": "CBuffer", "Stages": ["Compute"] })",
        R"({ "Name": "gTextures", "Binding": 0, "Count": 4, "Type": "Texture", "Stages": ["Compute"] })"));

    render::HlslShaderDesc reflection{};
    reflection.BoundResources.push_back(MakeHlslBind(
        "gTextures", render::HlslShaderInputType::TEXTURE, 0, 0, 8));

    ShaderAssetDiagnostic diag{};
    EXPECT_FALSE(ValidateShaderReflection(
        desc.Passes.front(), render::ShaderStage::Compute, reflection, diag));
    EXPECT_NE(diag.Message.find("count mismatch"), string::npos);
}

// ==================== 诊断上下文 ====================

TEST(ShaderAssetTest, DiagnosticCarriesLocatableContext) {
    const ShaderAssetDiagnostic diag = ParseFail(Mutate(
        kForwardManifest,
        R"({ "Name": "gNormalMap",     "Binding": 3, "Type": "Texture", "Stages": ["Pixel"] })",
        R"({ "Name": "gNormalMap",     "Binding": 3, "Type": "Texture", "Stages": ["Pixel"], "Residency": "RootDescriptor" })"));
    EXPECT_EQ(diag.PassName, "Forward");
    EXPECT_EQ(diag.BindingName, "gNormalMap");
    EXPECT_EQ(diag.Group.value_or(999u), 2u);
    EXPECT_EQ(diag.Binding.value_or(999u), 3u);

    const string text = diag.ToString();
    EXPECT_NE(text.find("Forward"), string::npos);
    EXPECT_NE(text.find("gNormalMap"), string::npos);
    EXPECT_NE(text.find("group=2"), string::npos);
}

namespace {

/// 每个测试独占一个临时目录, 析构时递归删除。
class TempDir {
public:
    TempDir() {
        static std::atomic<uint32_t> counter{0};
        std::error_code error;
        const uint32_t id = counter.fetch_add(1);
        _path = std::filesystem::temp_directory_path(error) /
                fmt::format("radray_shader_artifact_{}_{}", ::testing::UnitTest::GetInstance()->random_seed(), id);
        std::filesystem::remove_all(_path, error);
        std::filesystem::create_directories(_path, error);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(_path, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& Path() const noexcept { return _path; }
    std::filesystem::path operator/(std::string_view rel) const { return _path / std::filesystem::path{rel}; }

    void Write(std::string_view rel, std::string_view content) const {
        ASSERT_TRUE(WriteTextFile(_path / std::filesystem::path{rel}, content));
    }

private:
    std::filesystem::path _path;
};

vector<byte> MakeBytes(std::string_view text) {
    vector<byte> result;
    result.reserve(text.size());
    for (char c : text) {
        result.push_back(static_cast<byte>(c));
    }
    return result;
}

ShaderArtifactKeyParams BaseKeyParams() {
    return ShaderArtifactKeyParams{
        .SourceIdentity = ShaderHash{0x1111, 0x2222},
        .PassName = "forward",
        .Stage = render::ShaderStage::Vertex,
        .EntryPoint = "VSMain",
        .ShaderModel = render::HlslShaderModel::SM60,
        .Category = render::ShaderBlobCategory::DXIL,
        .Defines = {},
        .IsOptimize = true,
        .EnableUnbounded = true,
        .ToolchainHash = ShaderHash{0xaaaa, 0xbbbb}};
}

}  // namespace

// ============================ ShaderHash ============================

TEST(ShaderArtifactTest, HashHexRoundTrip) {
    const ShaderHash hash{0x0123456789abcdefull, 0xfedcba9876543210ull};
    const string hex = hash.ToHex();
    EXPECT_EQ(hex.size(), 32u);
    EXPECT_EQ(hex, "fedcba98765432100123456789abcdef");
    auto parsed = ShaderHash::FromHex(hex);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value(), hash);
}

TEST(ShaderArtifactTest, HashHexRejectsMalformed) {
    EXPECT_FALSE(ShaderHash::FromHex("").has_value());
    EXPECT_FALSE(ShaderHash::FromHex("abc").has_value());
    EXPECT_FALSE(ShaderHash::FromHex(string(31, 'a')).has_value());
    EXPECT_FALSE(ShaderHash::FromHex(string(33, 'a')).has_value());
    EXPECT_FALSE(ShaderHash::FromHex(string(31, 'a') + "z").has_value());
}

TEST(ShaderArtifactTest, HashHexAcceptsUppercase) {
    auto lower = ShaderHash::FromHex("fedcba98765432100123456789abcdef");
    auto upper = ShaderHash::FromHex("FEDCBA98765432100123456789ABCDEF");
    ASSERT_TRUE(lower.has_value());
    ASSERT_TRUE(upper.has_value());
    EXPECT_EQ(lower.value(), upper.value());
}

TEST(ShaderArtifactTest, ByteHashIsStableAndSensitive) {
    const vector<byte> a = MakeBytes("hello world");
    const vector<byte> b = MakeBytes("hello worlds");
    const vector<byte> c = MakeBytes("hello world");
    EXPECT_EQ(HashShaderBytes(a), HashShaderBytes(c));
    EXPECT_NE(HashShaderBytes(a), HashShaderBytes(b));
    EXPECT_FALSE(HashShaderBytes(a).IsZero());
}

TEST(ShaderArtifactTest, ByteHashDistinguishesSplitBoundaries) {
    // 长度前缀应使 ("ab") 与 ("a","b") 之类的拼接不相撞。
    EXPECT_NE(HashShaderBytes(MakeBytes("ab")), HashShaderBytes(MakeBytes("ba")));
    EXPECT_NE(HashShaderBytes(MakeBytes("a")), HashShaderBytes(MakeBytes("aa")));
}

// ============================ artifact key ============================

TEST(ShaderArtifactTest, KeyIsDeterministic) {
    EXPECT_EQ(ComputeShaderArtifactKey(BaseKeyParams()), ComputeShaderArtifactKey(BaseKeyParams()));
}

TEST(ShaderArtifactTest, KeyDependsOnEveryBytecodeAffectingInput) {
    const ShaderHash base = ComputeShaderArtifactKey(BaseKeyParams());

    auto sourceIdentity = BaseKeyParams();
    sourceIdentity.SourceIdentity = ShaderHash{0x9999, 0x2222};
    EXPECT_NE(ComputeShaderArtifactKey(sourceIdentity), base);

    auto pass = BaseKeyParams();
    pass.PassName = "shadow";
    EXPECT_NE(ComputeShaderArtifactKey(pass), base);

    auto stage = BaseKeyParams();
    stage.Stage = render::ShaderStage::Pixel;
    EXPECT_NE(ComputeShaderArtifactKey(stage), base);

    auto entry = BaseKeyParams();
    entry.EntryPoint = "VSMain2";
    EXPECT_NE(ComputeShaderArtifactKey(entry), base);

    auto sm = BaseKeyParams();
    sm.ShaderModel = render::HlslShaderModel::SM66;
    EXPECT_NE(ComputeShaderArtifactKey(sm), base);

    auto category = BaseKeyParams();
    category.Category = render::ShaderBlobCategory::SPIRV;
    EXPECT_NE(ComputeShaderArtifactKey(category), base);

    auto optimize = BaseKeyParams();
    optimize.IsOptimize = false;
    EXPECT_NE(ComputeShaderArtifactKey(optimize), base);

    auto unbounded = BaseKeyParams();
    unbounded.EnableUnbounded = false;
    EXPECT_NE(ComputeShaderArtifactKey(unbounded), base);

    auto toolchain = BaseKeyParams();
    toolchain.ToolchainHash = ShaderHash{0xcccc, 0xdddd};
    EXPECT_NE(ComputeShaderArtifactKey(toolchain), base);
}

TEST(ShaderArtifactTest, KeyIgnoresDefineOrder) {
    const vector<string> forward{"A=1", "B=2", "C"};
    const vector<string> reversed{"C", "B=2", "A=1"};
    auto lhs = BaseKeyParams();
    lhs.Defines = forward;
    auto rhs = BaseKeyParams();
    rhs.Defines = reversed;
    EXPECT_EQ(ComputeShaderArtifactKey(lhs), ComputeShaderArtifactKey(rhs));
}

TEST(ShaderArtifactTest, KeyIgnoresDuplicateDefines) {
    const vector<string> once{"A=1", "B"};
    const vector<string> twice{"A=1", "B", "A=1"};
    auto lhs = BaseKeyParams();
    lhs.Defines = once;
    auto rhs = BaseKeyParams();
    rhs.Defines = twice;
    EXPECT_EQ(ComputeShaderArtifactKey(lhs), ComputeShaderArtifactKey(rhs));
}

TEST(ShaderArtifactTest, KeyDependsOnDefineValues) {
    const vector<string> a{"QUALITY=1"};
    const vector<string> b{"QUALITY=2"};
    auto lhs = BaseKeyParams();
    lhs.Defines = a;
    auto rhs = BaseKeyParams();
    rhs.Defines = b;
    EXPECT_NE(ComputeShaderArtifactKey(lhs), ComputeShaderArtifactKey(rhs));
}

TEST(ShaderArtifactTest, KeyFromPassMatchesExplicitParams) {
    ShaderPassDesc pass;
    pass.Name = "forward";
    pass.Stages.push_back(ShaderStageDesc{render::ShaderStage::Vertex, "VSMain"});
    pass.ShaderModel = render::HlslShaderModel::SM60;
    pass.IsOptimize = true;
    pass.EnableUnbounded = true;

    auto fromPass = ComputeShaderArtifactKey(
        pass,
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        {},
        ShaderHash{0x1111, 0x2222},
        ShaderHash{0xaaaa, 0xbbbb});
    ASSERT_TRUE(fromPass.has_value());
    EXPECT_EQ(fromPass.value(), ComputeShaderArtifactKey(BaseKeyParams()));
}

TEST(ShaderArtifactTest, KeyFromPassFailsForUndeclaredStage) {
    ShaderPassDesc pass;
    pass.Name = "forward";
    pass.Stages.push_back(ShaderStageDesc{render::ShaderStage::Vertex, "VSMain"});
    auto key = ComputeShaderArtifactKey(
        pass,
        render::ShaderStage::Compute,
        render::ShaderBlobCategory::DXIL,
        {},
        ShaderHash{},
        ShaderHash{});
    EXPECT_FALSE(key.has_value());
}

TEST(ShaderArtifactTest, KeyReflectsManifestCompileOptions) {
    // IsOptimize / EnableUnbounded 进 manifest 的理由: 它们改变字节码, 必须换 key。
    ShaderPassDesc pass;
    pass.Name = "forward";
    pass.Stages.push_back(ShaderStageDesc{render::ShaderStage::Vertex, "VSMain"});
    pass.IsOptimize = true;
    auto optimized = ComputeShaderArtifactKey(
        pass, render::ShaderStage::Vertex, render::ShaderBlobCategory::DXIL, {}, ShaderHash{}, ShaderHash{});
    pass.IsOptimize = false;
    auto debug = ComputeShaderArtifactKey(
        pass, render::ShaderStage::Vertex, render::ShaderBlobCategory::DXIL, {}, ShaderHash{}, ShaderHash{});
    ASSERT_TRUE(optimized.has_value());
    ASSERT_TRUE(debug.has_value());
    EXPECT_NE(optimized.value(), debug.value());
}

// ============================ 源码身份 ============================

TEST(ShaderArtifactTest, SourceIdentityCoversIncludeClosure) {
    TempDir root;
    root.Write("main.hlsl", "#include \"common.hlsl\"\nfloat4 VSMain() { return Helper(); }\n");
    root.Write("common.hlsl", "float4 Helper() { return 1; }\n");

    ShaderAssetDiagnostic diag;
    auto identity = ComputeShaderSourceIdentity(root.Path(), "main.hlsl", diag);
    ASSERT_TRUE(identity.has_value()) << diag.Message;
    EXPECT_EQ(identity->Dependencies.size(), 2u);
    EXPECT_TRUE(std::ranges::find(identity->Dependencies, "main.hlsl") != identity->Dependencies.end());
    EXPECT_TRUE(std::ranges::find(identity->Dependencies, "common.hlsl") != identity->Dependencies.end());
    EXPECT_FALSE(identity->Hash.IsZero());
}

TEST(ShaderArtifactTest, SourceIdentityChangesWhenIncludeChanges) {
    TempDir root;
    root.Write("main.hlsl", "#include \"common.hlsl\"\n");
    root.Write("common.hlsl", "float4 Helper() { return 1; }\n");
    ShaderAssetDiagnostic diag;
    auto before = ComputeShaderSourceIdentity(root.Path(), "main.hlsl", diag);
    ASSERT_TRUE(before.has_value()) << diag.Message;

    root.Write("common.hlsl", "float4 Helper() { return 2; }\n");
    auto after = ComputeShaderSourceIdentity(root.Path(), "main.hlsl", diag);
    ASSERT_TRUE(after.has_value()) << diag.Message;
    EXPECT_NE(before->Hash, after->Hash);
}

TEST(ShaderArtifactTest, SourceIdentityIsStableAcrossRepeatedCalls) {
    TempDir root;
    root.Write("main.hlsl", "#include \"a.hlsl\"\n#include \"b.hlsl\"\n");
    root.Write("a.hlsl", "// a\n");
    root.Write("b.hlsl", "// b\n");
    ShaderAssetDiagnostic diag;
    auto first = ComputeShaderSourceIdentity(root.Path(), "main.hlsl", diag);
    auto second = ComputeShaderSourceIdentity(root.Path(), "main.hlsl", diag);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->Hash, second->Hash);
    EXPECT_EQ(first->Dependencies, second->Dependencies);
}

TEST(ShaderArtifactTest, SourceIdentityHandlesDiamondIncludes) {
    TempDir root;
    root.Write("main.hlsl", "#include \"a.hlsl\"\n#include \"b.hlsl\"\n");
    root.Write("a.hlsl", "#include \"common.hlsl\"\n");
    root.Write("b.hlsl", "#include \"common.hlsl\"\n");
    root.Write("common.hlsl", "// common\n");
    ShaderAssetDiagnostic diag;
    auto identity = ComputeShaderSourceIdentity(root.Path(), "main.hlsl", diag);
    ASSERT_TRUE(identity.has_value()) << diag.Message;
    // common.hlsl 只应计入一次。
    EXPECT_EQ(identity->Dependencies.size(), 4u);
    EXPECT_EQ(std::ranges::count(identity->Dependencies, "common.hlsl"), 1);
}

TEST(ShaderArtifactTest, SourceIdentityToleratesCyclicIncludes) {
    TempDir root;
    root.Write("a.hlsl", "#include \"b.hlsl\"\n");
    root.Write("b.hlsl", "#include \"a.hlsl\"\n");
    ShaderAssetDiagnostic diag;
    auto identity = ComputeShaderSourceIdentity(root.Path(), "a.hlsl", diag);
    ASSERT_TRUE(identity.has_value()) << diag.Message;
    EXPECT_EQ(identity->Dependencies.size(), 2u);
}

TEST(ShaderArtifactTest, SourceIdentityIgnoresCommentedIncludes) {
    TempDir root;
    root.Write("main.hlsl", "// #include \"ghost.hlsl\"\n/* #include \"ghost2.hlsl\" */\n// body\n");
    ShaderAssetDiagnostic diag;
    auto identity = ComputeShaderSourceIdentity(root.Path(), "main.hlsl", diag);
    ASSERT_TRUE(identity.has_value()) << diag.Message;
    EXPECT_EQ(identity->Dependencies.size(), 1u);
}

TEST(ShaderArtifactTest, SourceIdentityCountsConditionallyExcludedIncludes) {
    // 刻意不求解 #if: 过度失效优于漏失效。
    TempDir root;
    root.Write("main.hlsl", "#if 0\n#include \"cold.hlsl\"\n#endif\n");
    root.Write("cold.hlsl", "// cold\n");
    ShaderAssetDiagnostic diag;
    auto identity = ComputeShaderSourceIdentity(root.Path(), "main.hlsl", diag);
    ASSERT_TRUE(identity.has_value()) << diag.Message;
    EXPECT_EQ(identity->Dependencies.size(), 2u);
}

TEST(ShaderArtifactTest, SourceIdentityAcceptsAngleBracketIncludes) {
    TempDir root;
    root.Write("main.hlsl", "#include <common.hlsl>\n");
    root.Write("common.hlsl", "// common\n");
    ShaderAssetDiagnostic diag;
    auto identity = ComputeShaderSourceIdentity(root.Path(), "main.hlsl", diag);
    ASSERT_TRUE(identity.has_value()) << diag.Message;
    EXPECT_EQ(identity->Dependencies.size(), 2u);
}

TEST(ShaderArtifactTest, SourceIdentityAcceptsSubdirectoryIncludes) {
    TempDir root;
    std::error_code error;
    std::filesystem::create_directories(root / "forward_pipeline", error);
    root.Write("forward_pipeline/pass.hlsl", "#include \"common.hlsl\"\n");
    root.Write("common.hlsl", "// common\n");
    ShaderAssetDiagnostic diag;
    auto identity = ComputeShaderSourceIdentity(root.Path(), "forward_pipeline/pass.hlsl", diag);
    ASSERT_TRUE(identity.has_value()) << diag.Message;
    EXPECT_EQ(identity->Dependencies.size(), 2u);
    // 依赖路径用 generic 分隔符, 保证跨平台哈希一致。
    EXPECT_TRUE(std::ranges::find(identity->Dependencies, "forward_pipeline/pass.hlsl") !=
                identity->Dependencies.end());
}

TEST(ShaderArtifactTest, SourceIdentityRejectsMissingSource) {
    TempDir root;
    ShaderAssetDiagnostic diag;
    auto identity = ComputeShaderSourceIdentity(root.Path(), "missing.hlsl", diag);
    EXPECT_FALSE(identity.has_value());
    EXPECT_NE(diag.Message.find("missing"), string::npos);
}

TEST(ShaderArtifactTest, SourceIdentityRejectsMissingInclude) {
    TempDir root;
    root.Write("main.hlsl", "#include \"absent.hlsl\"\n");
    ShaderAssetDiagnostic diag;
    auto identity = ComputeShaderSourceIdentity(root.Path(), "main.hlsl", diag);
    EXPECT_FALSE(identity.has_value());
}

TEST(ShaderArtifactTest, SourceIdentityRejectsEscapingRoot) {
    TempDir root;
    std::error_code error;
    std::filesystem::create_directories(root / "inner", error);
    root.Write("inner/main.hlsl", "// body\n");
    root.Write("outside.hlsl", "// outside\n");
    ShaderAssetDiagnostic diag;
    auto identity = ComputeShaderSourceIdentity(root / "inner", "../outside.hlsl", diag);
    EXPECT_FALSE(identity.has_value());
}

TEST(ShaderArtifactTest, SourceIdentityRejectsMacroInclude) {
    TempDir root;
    root.Write("main.hlsl", "#define P \"x.hlsl\"\n#include P\n");
    ShaderAssetDiagnostic diag;
    auto identity = ComputeShaderSourceIdentity(root.Path(), "main.hlsl", diag);
    EXPECT_FALSE(identity.has_value());
    EXPECT_NE(diag.Message.find("macro-based"), string::npos);
}

TEST(ShaderArtifactTest, SourceIdentityRejectsMissingRoot) {
    ShaderAssetDiagnostic diag;
    auto identity = ComputeShaderSourceIdentity("Z:/definitely/not/here", "main.hlsl", diag);
    EXPECT_FALSE(identity.has_value());
    EXPECT_NE(diag.Message.find("unavailable"), string::npos);
}

// ============================ 路径约定 ============================

TEST(ShaderArtifactTest, ArtifactDirectoryStripsAllExtensions) {
    // .shader.json 是两级后缀, 必须全部剥掉。
    EXPECT_EQ(
        GetShaderArtifactDirectory("a/forward_pass.shader.json").generic_string(),
        "a/forward_pass");
    EXPECT_EQ(GetShaderArtifactDirectory("forward_pass.shader.json").generic_string(), "forward_pass");
    EXPECT_EQ(GetShaderArtifactDirectory("a/b.json").generic_string(), "a/b");
    EXPECT_EQ(GetShaderArtifactDirectory("a/b").generic_string(), "a/b");
}

TEST(ShaderArtifactTest, BlobPathIsCategoryScopedAndContentAddressed) {
    const ShaderHash key{0x0123456789abcdefull, 0xfedcba9876543210ull};
    EXPECT_EQ(
        MakeShaderArtifactBlobPath(render::ShaderBlobCategory::DXIL, key),
        "dxil/fedcba98765432100123456789abcdef.bin");
    EXPECT_EQ(
        MakeShaderArtifactBlobPath(render::ShaderBlobCategory::SPIRV, key),
        "spirv/fedcba98765432100123456789abcdef.bin");
}

// ============================ blob 读写 ============================

namespace {

ShaderArtifactEntry MakeEntry(std::span<const byte> bytecode, ShaderHash key) {
    ShaderArtifactEntry entry;
    entry.Key = key;
    entry.PassName = "forward";
    entry.Stage = render::ShaderStage::Vertex;
    entry.EntryPoint = "VSMain";
    entry.Category = render::ShaderBlobCategory::DXIL;
    entry.BlobPath = MakeShaderArtifactBlobPath(entry.Category, key);
    entry.BytecodeHash = HashShaderBytes(bytecode);
    entry.BytecodeSize = static_cast<uint32_t>(bytecode.size());
    return entry;
}

}  // namespace

TEST(ShaderArtifactTest, BlobRoundTrip) {
    TempDir dir;
    const vector<byte> bytecode = MakeBytes("DXBCfake-bytecode-payload");
    const ShaderHash key{0x1234, 0x5678};
    const ShaderArtifactEntry entry = MakeEntry(bytecode, key);
    const std::filesystem::path path = dir / entry.BlobPath;

    ASSERT_TRUE(WriteShaderArtifactBlob(path, entry, bytecode));
    ShaderAssetDiagnostic diag;
    auto blob = ReadShaderArtifactBlob(path, diag);
    ASSERT_TRUE(blob.has_value()) << diag.Message;
    EXPECT_EQ(blob->Key, key);
    EXPECT_EQ(blob->Stage, render::ShaderStage::Vertex);
    EXPECT_EQ(blob->Category, render::ShaderBlobCategory::DXIL);
    EXPECT_EQ(blob->Bytecode, bytecode);
}

TEST(ShaderArtifactTest, BlobWriteCreatesParentDirectories) {
    TempDir dir;
    const vector<byte> bytecode = MakeBytes("payload");
    const ShaderArtifactEntry entry = MakeEntry(bytecode, ShaderHash{1, 2});
    // "dxil/" 子目录不存在, 应由写入自动创建。
    ASSERT_TRUE(WriteShaderArtifactBlob(dir / entry.BlobPath, entry, bytecode));
    EXPECT_TRUE(std::filesystem::exists(dir / entry.BlobPath));
}

TEST(ShaderArtifactTest, BlobBytecodeIsFourByteAligned) {
    // Vulkan 侧会把字节码指针 bit_cast 成 const uint32_t*, 故要求 4 字节对齐。
    TempDir dir;
    const vector<byte> bytecode = MakeBytes("0123456789abcdef0123");
    const ShaderArtifactEntry entry = MakeEntry(bytecode, ShaderHash{7, 8});
    ASSERT_TRUE(WriteShaderArtifactBlob(dir / entry.BlobPath, entry, bytecode));
    ShaderAssetDiagnostic diag;
    auto blob = ReadShaderArtifactBlob(dir / entry.BlobPath, diag);
    ASSERT_TRUE(blob.has_value()) << diag.Message;
    EXPECT_EQ(reinterpret_cast<uintptr_t>(blob->Bytecode.data()) % 4u, 0u);
}

TEST(ShaderArtifactTest, BlobRejectsEmptyBytecode) {
    TempDir dir;
    const ShaderArtifactEntry entry = MakeEntry({}, ShaderHash{1, 1});
    EXPECT_FALSE(WriteShaderArtifactBlob(dir / "dxil/x.bin", entry, {}));
}

TEST(ShaderArtifactTest, BlobRejectsMissingFile) {
    TempDir dir;
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(ReadShaderArtifactBlob(dir / "nope.bin", diag).has_value());
    EXPECT_NE(diag.Message.find("failed to read"), string::npos);
}

TEST(ShaderArtifactTest, BlobRejectsBadMagic) {
    TempDir dir;
    const std::filesystem::path path = dir / "bad.bin";
    ASSERT_TRUE(WriteTextFile(path, "not a shader blob at all, but long enough"));
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(ReadShaderArtifactBlob(path, diag).has_value());
    EXPECT_NE(diag.Message.find("magic"), string::npos);
}

TEST(ShaderArtifactTest, BlobRejectsTruncatedFile) {
    TempDir dir;
    const vector<byte> bytecode = MakeBytes("some-bytecode");
    const ShaderArtifactEntry entry = MakeEntry(bytecode, ShaderHash{3, 4});
    const std::filesystem::path path = dir / entry.BlobPath;
    ASSERT_TRUE(WriteShaderArtifactBlob(path, entry, bytecode));

    auto full = ReadBinaryFile(path);
    ASSERT_TRUE(full.has_value());
    const std::span<const byte> head{full->data(), full->size() / 2};
    ASSERT_TRUE(WriteBinaryFile(path, head));

    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(ReadShaderArtifactBlob(path, diag).has_value());
}

TEST(ShaderArtifactTest, BlobDetectsCorruptedBytecode) {
    TempDir dir;
    const vector<byte> bytecode = MakeBytes("payload-to-be-corrupted");
    const ShaderArtifactEntry entry = MakeEntry(bytecode, ShaderHash{5, 6});
    const std::filesystem::path path = dir / entry.BlobPath;
    ASSERT_TRUE(WriteShaderArtifactBlob(path, entry, bytecode));

    auto full = ReadBinaryFile(path);
    ASSERT_TRUE(full.has_value());
    // 翻转最后一个字节, 内容哈希校验应捕获。
    full->back() = static_cast<byte>(static_cast<uint8_t>(full->back()) ^ 0xffu);
    ASSERT_TRUE(WriteBinaryFile(path, full.value()));

    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(ReadShaderArtifactBlob(path, diag).has_value());
    EXPECT_NE(diag.Message.find("content hash"), string::npos);
}

// ============================ index.json ============================

namespace {

ShaderArtifactIndex MakeIndex() {
    ShaderArtifactIndex index;
    index.AssetName = "forward_pass";
    index.Sources.push_back(ShaderArtifactSource{"forward.hlsl", ShaderHash{0x1111, 0x2222}});
    index.ToolchainHash = ShaderHash{0xaaaa, 0xbbbb};

    ShaderArtifactEntry vs;
    vs.Key = ShaderHash{1, 2};
    vs.PassName = "forward";
    vs.Source = "forward.hlsl";
    vs.Stage = render::ShaderStage::Vertex;
    vs.EntryPoint = "VSMain";
    vs.Category = render::ShaderBlobCategory::DXIL;
    vs.BlobPath = MakeShaderArtifactBlobPath(vs.Category, vs.Key);
    vs.BytecodeHash = ShaderHash{3, 4};
    vs.BytecodeSize = 128;
    index.Entries.push_back(vs);

    ShaderArtifactEntry ps;
    ps.Key = ShaderHash{5, 6};
    ps.PassName = "forward";
    ps.Source = "forward.hlsl";
    ps.Stage = render::ShaderStage::Pixel;
    ps.EntryPoint = "PSMain";
    ps.Category = render::ShaderBlobCategory::SPIRV;
    ps.BlobPath = MakeShaderArtifactBlobPath(ps.Category, ps.Key);
    ps.BytecodeHash = ShaderHash{7, 8};
    ps.BytecodeSize = 256;
    index.Entries.push_back(ps);
    return index;
}

/// 一个源文件加一条 entry 的最小 index JSON。用于精确构造负例。
string MakeIndexJson(
    uint32_t formatVersion,
    std::string_view sourcesJson,
    std::string_view entriesJson) {
    return fmt::format(
        R"({{"FormatVersion":{},"AssetName":"x","Sources":{},"ToolchainHash":"{}","Entries":{}}})",
        formatVersion,
        sourcesJson,
        ShaderHash{3, 4}.ToHex(),
        entriesJson);
}

string MakeOneSourceJson() {
    return fmt::format(
        R"([{{"Path":"a.hlsl","Identity":"{}"}}])",
        ShaderHash{1, 2}.ToHex());
}

}  // namespace

TEST(ShaderArtifactTest, IndexRoundTrip) {
    const ShaderArtifactIndex original = MakeIndex();
    auto json = SerializeShaderArtifactIndex(original);
    ASSERT_TRUE(json.has_value());

    ShaderAssetDiagnostic diag;
    auto parsed = ParseShaderArtifactIndex(json.value(), diag);
    ASSERT_TRUE(parsed.has_value()) << diag.Message;
    EXPECT_EQ(parsed->AssetName, original.AssetName);
    EXPECT_EQ(parsed->Sources, original.Sources);
    EXPECT_EQ(parsed->ToolchainHash, original.ToolchainHash);
    ASSERT_EQ(parsed->Entries.size(), original.Entries.size());
    EXPECT_EQ(parsed->Entries, original.Entries);
}

TEST(ShaderArtifactTest, IndexUsesStringEnums) {
    auto json = SerializeShaderArtifactIndex(MakeIndex());
    ASSERT_TRUE(json.has_value());
    // index.json 是人可读产物清单, 枚举必须是字符串。
    EXPECT_NE(json->find("\"Vertex\""), string::npos);
    EXPECT_NE(json->find("\"DXIL\""), string::npos);
    EXPECT_NE(json->find("\"SPIRV\""), string::npos);
}

TEST(ShaderArtifactTest, IndexFindLocatesEntryByKey) {
    const ShaderArtifactIndex index = MakeIndex();
    auto found = index.Find(ShaderHash{5, 6});
    ASSERT_TRUE(found.HasValue());
    EXPECT_EQ(found.Get()->EntryPoint, "PSMain");
    EXPECT_FALSE(index.Find(ShaderHash{999, 999}).HasValue());
}

TEST(ShaderArtifactTest, IndexLoadsFromDisk) {
    TempDir dir;
    auto json = SerializeShaderArtifactIndex(MakeIndex());
    ASSERT_TRUE(json.has_value());
    const std::filesystem::path path = dir / "index.json";
    ASSERT_TRUE(WriteTextFile(path, json.value()));

    ShaderAssetDiagnostic diag;
    auto loaded = LoadShaderArtifactIndex(path, diag);
    ASSERT_TRUE(loaded.has_value()) << diag.Message;
    EXPECT_EQ(loaded->Entries.size(), 2u);
}

TEST(ShaderArtifactTest, IndexRejectsMissingFile) {
    TempDir dir;
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(LoadShaderArtifactIndex(dir / "absent.json", diag).has_value());
}

TEST(ShaderArtifactTest, IndexRejectsMalformedJson) {
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(ParseShaderArtifactIndex("{ not json", diag).has_value());
}

TEST(ShaderArtifactTest, IndexRejectsNonObjectRoot) {
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(ParseShaderArtifactIndex("[]", diag).has_value());
}

TEST(ShaderArtifactTest, IndexRejectsUnknownFormatVersion) {
    ShaderAssetDiagnostic diag;
    const string json = MakeIndexJson(
        kShaderArtifactFormatVersion + 1,
        MakeOneSourceJson(),
        "[]");
    EXPECT_FALSE(ParseShaderArtifactIndex(json, diag).has_value());
    EXPECT_NE(diag.Message.find("FormatVersion"), string::npos);
}

TEST(ShaderArtifactTest, IndexRejectsMissingFormatVersion) {
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(ParseShaderArtifactIndex(R"({"AssetName":"x","Entries":[]})", diag).has_value());
}

TEST(ShaderArtifactTest, IndexRejectsMalformedSourceIdentity) {
    ShaderAssetDiagnostic diag;
    const string json = MakeIndexJson(
        kShaderArtifactFormatVersion,
        R"([{"Path":"a.hlsl","Identity":"zz"}])",
        "[]");
    EXPECT_FALSE(ParseShaderArtifactIndex(json, diag).has_value());
    EXPECT_NE(diag.Message.find("Identity"), string::npos);
}

TEST(ShaderArtifactTest, IndexRejectsMissingSources) {
    // Sources 是 key 计算的输入之一, 缺了它任何 entry 都无法被查找。
    ShaderAssetDiagnostic diag;
    const string json = MakeIndexJson(kShaderArtifactFormatVersion, "[]", "[]");
    EXPECT_FALSE(ParseShaderArtifactIndex(json, diag).has_value());
    EXPECT_NE(diag.Message.find("Sources"), string::npos);
}

TEST(ShaderArtifactTest, IndexRejectsDuplicateSources) {
    ShaderAssetDiagnostic diag;
    const string json = MakeIndexJson(
        kShaderArtifactFormatVersion,
        fmt::format(
            R"([{{"Path":"a.hlsl","Identity":"{}"}},{{"Path":"a.hlsl","Identity":"{}"}}])",
            ShaderHash{1, 2}.ToHex(),
            ShaderHash{5, 6}.ToHex()),
        "[]");
    EXPECT_FALSE(ParseShaderArtifactIndex(json, diag).has_value());
    EXPECT_NE(diag.Message.find("duplicate source"), string::npos);
}

TEST(ShaderArtifactTest, IndexRejectsEntryWithUnrecordedSource) {
    // entry 的 key 是按其源文件身份算的; Sources 里没有该源文件说明 index 自相矛盾。
    ShaderAssetDiagnostic diag;
    ShaderArtifactIndex index = MakeIndex();
    index.Entries[0].Source = "other.hlsl";
    auto json = SerializeShaderArtifactIndex(index);
    ASSERT_TRUE(json.has_value());
    EXPECT_FALSE(ParseShaderArtifactIndex(json.value(), diag).has_value());
    EXPECT_NE(diag.Message.find("other.hlsl"), string::npos);
}

TEST(ShaderArtifactTest, IndexFindsSourceIdentityByPath) {
    const ShaderArtifactIndex index = MakeIndex();
    auto found = index.FindSourceIdentity("forward.hlsl");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found.value(), ShaderHash(0x1111, 0x2222));
    EXPECT_FALSE(index.FindSourceIdentity("absent.hlsl").has_value());
}

TEST(ShaderArtifactTest, IndexRejectsUnknownStage) {
    ShaderAssetDiagnostic diag;
    auto json = SerializeShaderArtifactIndex(MakeIndex());
    ASSERT_TRUE(json.has_value());
    string mutated = json.value();
    const size_t pos = mutated.find("\"Vertex\"");
    ASSERT_NE(pos, string::npos);
    mutated.replace(pos, 8, "\"Geometry\"");
    EXPECT_FALSE(ParseShaderArtifactIndex(mutated, diag).has_value());
    EXPECT_NE(diag.Message.find("Stage"), string::npos);
}

TEST(ShaderArtifactTest, IndexRejectsDuplicateKeys) {
    ShaderArtifactIndex index = MakeIndex();
    index.Entries[1].Key = index.Entries[0].Key;
    auto json = SerializeShaderArtifactIndex(index);
    ASSERT_TRUE(json.has_value());
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(ParseShaderArtifactIndex(json.value(), diag).has_value());
    EXPECT_NE(diag.Message.find("duplicate"), string::npos);
}

TEST(ShaderArtifactTest, IndexRejectsZeroBytecodeSize) {
    ShaderArtifactIndex index = MakeIndex();
    index.Entries[0].BytecodeSize = 0;
    auto json = SerializeShaderArtifactIndex(index);
    ASSERT_TRUE(json.has_value());
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(ParseShaderArtifactIndex(json.value(), diag).has_value());
    EXPECT_NE(diag.Message.find("BytecodeSize"), string::npos);
}

TEST(ShaderArtifactTest, IndexRejectsEmptyBlobPath) {
    ShaderArtifactIndex index = MakeIndex();
    index.Entries[0].BlobPath.clear();
    auto json = SerializeShaderArtifactIndex(index);
    ASSERT_TRUE(json.has_value());
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(ParseShaderArtifactIndex(json.value(), diag).has_value());
    EXPECT_NE(diag.Message.find("BlobPath"), string::npos);
}

TEST(ShaderArtifactTest, IndexAcceptsEmptyEntries) {
    ShaderArtifactIndex index = MakeIndex();
    index.Entries.clear();
    auto json = SerializeShaderArtifactIndex(index);
    ASSERT_TRUE(json.has_value());
    ShaderAssetDiagnostic diag;
    auto parsed = ParseShaderArtifactIndex(json.value(), diag);
    ASSERT_TRUE(parsed.has_value()) << diag.Message;
    EXPECT_TRUE(parsed->Entries.empty());
}

TEST(ShaderArtifactTest, EntryKeywordsRoundTrip) {
    ShaderArtifactIndex index = MakeIndex();
    index.Entries[0].Keywords = {"_ALPHATEST_ON", "_NORMAL_MAP"};
    auto json = SerializeShaderArtifactIndex(index);
    ASSERT_TRUE(json.has_value());
    ShaderAssetDiagnostic diag;
    auto parsed = ParseShaderArtifactIndex(json.value(), diag);
    ASSERT_TRUE(parsed.has_value()) << diag.Message;
    ASSERT_EQ(parsed->Entries.size(), 2u);
    EXPECT_EQ(parsed->Entries[0].Keywords, index.Entries[0].Keywords);
    // 第二条没写 Keywords, 解析后应为空而非缺失。
    EXPECT_TRUE(parsed->Entries[1].Keywords.empty());
}

TEST(ShaderArtifactTest, IndexTreatsMissingKeywordsAsEmpty) {
    // 空 Keywords 不写入 index; 解析时缺失字段必须与空数组等价。
    const ShaderArtifactIndex index = MakeIndex();
    auto json = SerializeShaderArtifactIndex(index);
    ASSERT_TRUE(json.has_value());
    EXPECT_EQ(json->find("Keywords"), string::npos);
    ShaderAssetDiagnostic diag;
    auto parsed = ParseShaderArtifactIndex(json.value(), diag);
    ASSERT_TRUE(parsed.has_value()) << diag.Message;
    for (const ShaderArtifactEntry& entry : parsed->Entries) {
        EXPECT_TRUE(entry.Keywords.empty());
    }
}

TEST(ShaderArtifactTest, KeywordsDoNotAffectTheKey) {
    // Keywords 只是可读身份, 查找靠 Key。同一个 key 的两条 entry 只是记录不同,
    // 不构成不同的产物。
    const std::array<string, 1> defines{"_NORMAL_MAP"};
    const ShaderHash a = ComputeShaderArtifactKey(ShaderArtifactKeyParams{
        .SourceIdentity = ShaderHash{1, 2},
        .PassName = "forward",
        .Stage = render::ShaderStage::Pixel,
        .EntryPoint = "PSMain",
        .Category = render::ShaderBlobCategory::DXIL,
        .Defines = defines,
        .ToolchainHash = ShaderHash{3, 4}});
    const ShaderHash b = ComputeShaderArtifactKey(ShaderArtifactKeyParams{
        .SourceIdentity = ShaderHash{1, 2},
        .PassName = "forward",
        .Stage = render::ShaderStage::Pixel,
        .EntryPoint = "PSMain",
        .Category = render::ShaderBlobCategory::DXIL,
        .Defines = defines,
        .ToolchainHash = ShaderHash{3, 4}});
    EXPECT_EQ(a, b);
}

TEST(ShaderArtifactTest, RejectsNonArrayKeywords) {
    // 手写 JSON 而非改序列化输出: 后者的排版会随 writer 变化, 断言会变脆。
    const string json = fmt::format(
        R"JSON({{
      "FormatVersion": {},
      "AssetName": "Sample",
      "Sources": [
        {{ "Path": "forward.hlsl", "Identity": "00000000000022220000000000001111" }}
      ],
      "ToolchainHash": "0000000000000bbb0000000000000aaa",
      "Entries": [
        {{
          "Key": "00000000000000020000000000000001",
          "PassName": "forward",
          "Source": "forward.hlsl",
          "Stage": "Vertex",
          "EntryPoint": "VSMain",
          "Category": "DXIL",
          "BlobPath": "dxil/00000000000000020000000000000001.bin",
          "BytecodeHash": "00000000000000040000000000000003",
          "BytecodeSize": 128,
          "Keywords": "_NORMAL_MAP"
        }}
      ]
    }})JSON",
        kShaderArtifactFormatVersion);
    ShaderAssetDiagnostic diag;
    EXPECT_FALSE(ParseShaderArtifactIndex(json, diag).has_value());
    EXPECT_NE(diag.Message.find("Keywords"), string::npos);
}

namespace {

/// 一套完整的临时 shader 工作区: shaderlib 根 + manifest。
class ShaderWorkspace {
public:
    ShaderWorkspace() {
        static std::atomic<uint32_t> counter{0};
        std::error_code error;
        _base = std::filesystem::temp_directory_path(error) /
                fmt::format("radray_shader_resolver_{}", counter.fetch_add(1));
        std::filesystem::remove_all(_base, error);
        std::filesystem::create_directories(_base / "shaderlib", error);
    }
    ~ShaderWorkspace() {
        std::error_code error;
        std::filesystem::remove_all(_base, error);
    }
    ShaderWorkspace(const ShaderWorkspace&) = delete;
    ShaderWorkspace& operator=(const ShaderWorkspace&) = delete;

    std::filesystem::path Root() const { return _base / "shaderlib"; }
    std::filesystem::path ManifestPath() const { return _base / "shaderlib" / "test.shader.json"; }
    std::filesystem::path ArtifactDir() const { return GetShaderArtifactDirectory(ManifestPath()); }

    void WriteSource(std::string_view rel, std::string_view content) const {
        ASSERT_TRUE(WriteTextFile(Root() / std::filesystem::path{rel}, content));
    }
    void WriteManifest(std::string_view content) const {
        ASSERT_TRUE(WriteTextFile(ManifestPath(), content));
    }

private:
    std::filesystem::path _base;
};

constexpr std::string_view kSimpleHlsl = R"(
struct VSOut {
    float4 Position : SV_POSITION;
};

VSOut VSMain(float3 position : POSITION) {
    VSOut result;
    result.Position = float4(position, 1.0);
    return result;
}

float4 PSMain(VSOut input) : SV_TARGET {
    return float4(1.0, 0.0, 0.0, 1.0);
}
)";

string MakeManifest() {
    return fmt::format(
        R"({{
  "FormatVersion": {},
  "Name": "test",
  "Source": "test.hlsl",
  "Passes": [
    {{
      "Name": "main",
      "Source": "test.hlsl",
      "Stages": [
        {{ "Stage": "Vertex", "EntryPoint": "VSMain" }},
        {{ "Stage": "Pixel", "EntryPoint": "PSMain" }}
      ],
      "ShaderModel": "SM60",
      "IsOptimize": false,
      "EnableUnbounded": true,
      "VertexInput": {{
        "Buffers": [ {{ "Binding": 0, "ArrayStride": 12, "StepMode": "Vertex" }} ],
        "Attributes": [
          {{ "Semantic": "POSITION", "SemanticIndex": 0, "Format": "FLOAT32X3", "BufferBinding": 0, "Offset": 0 }}
        ]
      }}
    }}
  ]
}})",
        kShaderAssetFormatVersion);
}

/// 建好工作区并解析 manifest。
struct Fixture {
    ShaderWorkspace Workspace;
    ShaderAssetDesc Asset;

    Fixture() {
        Workspace.WriteSource("test.hlsl", kSimpleHlsl);
        Workspace.WriteManifest(MakeManifest());
        ShaderAssetDiagnostic diag;
        auto asset = ParseShaderAssetDesc(MakeManifest(), diag);
        if (asset.has_value()) {
            Asset = std::move(asset.value());
        } else {
            ADD_FAILURE() << "manifest parse failed: " << diag.Message;
        }
    }

    ShaderResolveConfig Config(ShaderArtifactStaleness staleness, bool allowJit) const {
        return ShaderResolveConfig{
            .ShaderRoot = Workspace.Root(),
            .ManifestPath = Workspace.ManifestPath(),
            .Staleness = staleness,
            .AllowJit = allowJit};
    }

    const ShaderPassDesc& Pass() const { return Asset.Passes.front(); }
};

}  // namespace

// ============================ 与后端无关的部分 ============================

TEST(ShaderResolverTest, ToolchainHashIsStableAndNonZero) {
    const ShaderHash first = GetShaderToolchainHash();
    const ShaderHash second = GetShaderToolchainHash();
    EXPECT_EQ(first, second);
    EXPECT_FALSE(first.IsZero());
}

TEST(ShaderResolverTest, SourceMustBeResolvedBeforeResolving) {
    Fixture fixture;
    ShaderPassDesc pass = fixture.Pass();
    pass.Source.clear();  // 资产级继承应由调用方先完成。
    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, false), nullptr};
    ShaderAssetDiagnostic diag;
    auto bytecode = resolver.Resolve(
        pass,
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    EXPECT_FALSE(bytecode.has_value());
    EXPECT_NE(diag.Message.find("source path is empty"), string::npos);
}

TEST(ShaderResolverTest, UndeclaredStageIsRejected) {
    Fixture fixture;
    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, false), nullptr};
    ShaderAssetDiagnostic diag;
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Compute,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    EXPECT_FALSE(bytecode.has_value());
    EXPECT_NE(diag.Message.find("entry point"), string::npos);
}

TEST(ShaderResolverTest, WithoutArtifactsOrJitResolveFails) {
    Fixture fixture;
    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, false), nullptr};
    EXPECT_FALSE(resolver.CanJit());
    ShaderAssetDiagnostic diag;
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    EXPECT_FALSE(bytecode.has_value());
    EXPECT_NE(diag.Message.find("JIT is unavailable"), string::npos);
}

TEST(ShaderResolverTest, SourceIdentityIsCached) {
    Fixture fixture;
    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, false), nullptr};
    ShaderAssetDiagnostic diag;
    auto first = resolver.GetSourceIdentity("test.hlsl", diag);
    auto second = resolver.GetSourceIdentity("test.hlsl", diag);
    ASSERT_TRUE(first.has_value()) << diag.Message;
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first.value(), second.value());
}

TEST(ShaderResolverTest, ResolverDoesNotOwnTheCompiler) {
    // ShaderResolver 只借用 Dxc*, 析构不应影响调用方的所有权。
    Fixture fixture;
    ShaderAssetDiagnostic diag;
    auto identity = ComputeShaderSourceIdentity(fixture.Workspace.Root(), "test.hlsl", diag);
    ASSERT_TRUE(identity.has_value()) << diag.Message;
    {
        ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, false), nullptr};
        EXPECT_FALSE(resolver.CanJit());
    }
    // resolver 已析构; 源码身份计算仍可独立进行, 说明没有共享状态被带走。
    auto again = ComputeShaderSourceIdentity(fixture.Workspace.Root(), "test.hlsl", diag);
    ASSERT_TRUE(again.has_value()) << diag.Message;
    EXPECT_EQ(identity->Hash, again->Hash);
}

#if defined(RADRAY_ENABLE_SHADER_JIT)

// ============================ 需要 DXC 的部分 ============================

namespace {

/// 测试持有 Dxc 的所有权; ShaderResolver 只借用它。
shared_ptr<render::Dxc> MakeDxc() {
    auto dxc = render::CreateDxc();
    if (!dxc.HasValue()) {
        return nullptr;
    }
    return dxc.Release();
}

ShaderCookOptions CookOptions(const Fixture& fixture, vector<render::ShaderBlobCategory> categories) {
    return ShaderCookOptions{
        .ShaderRoot = fixture.Workspace.Root(),
        .ManifestPath = fixture.Workspace.ManifestPath(),
        .Categories = std::move(categories),
        .ValidateReflection = true,
        .Incremental = true};
}

/// 只在 PS 里分支的 HLSL。VS 与 keyword 无关, 用来验证 stage 投影后的 blob 共享。
constexpr std::string_view kVariantHlsl = R"(
struct VSOut {
    float4 Position : SV_POSITION;
};

VSOut VSMain(float3 position : POSITION) {
    VSOut result;
    result.Position = float4(position, 1.0);
    return result;
}

float4 PSMain(VSOut input) : SV_TARGET {
#ifdef _ALPHATEST_ON
    return float4(0.0, 1.0, 0.0, 0.5);
#else
    return float4(1.0, 0.0, 0.0, 1.0);
#endif
}
)";

/// 一个可选的、只作用于 Pixel 的 keyword 组, 并声明烘焙它的全部取值。
string MakeVariantManifest() {
    return fmt::format(
        R"({{
  "FormatVersion": {},
  "Name": "test",
  "Source": "test.hlsl",
  "KeywordGroups": [
    {{ "Name": "AlphaTest", "Keywords": [ "_ALPHATEST_ON" ], "IsOptional": true, "Stages": [ "Pixel" ] }}
  ],
  "BakeVariants": {{
    "Rules": [ {{ "Expand": [ "AlphaTest" ] }} ]
  }},
  "Passes": [
    {{
      "Name": "main",
      "Source": "test.hlsl",
      "Stages": [
        {{ "Stage": "Vertex", "EntryPoint": "VSMain" }},
        {{ "Stage": "Pixel", "EntryPoint": "PSMain" }}
      ],
      "ShaderModel": "SM60",
      "IsOptimize": false,
      "EnableUnbounded": true,
      "VertexInput": {{
        "Buffers": [ {{ "Binding": 0, "ArrayStride": 12, "StepMode": "Vertex" }} ],
        "Attributes": [
          {{ "Semantic": "POSITION", "SemanticIndex": 0, "Format": "FLOAT32X3", "BufferBinding": 0, "Offset": 0 }}
        ]
      }}
    }}
  ]
}})",
        kShaderAssetFormatVersion);
}

/// 两个 pass 各用一个源文件。用于验证 index 按源文件记录身份。
constexpr std::string_view kShadowHlsl = R"(
float4 ShadowVSMain(float3 position : POSITION) : SV_POSITION {
    return float4(position, 1.0);
}
)";

string MakeMultiSourceManifest() {
    return fmt::format(
        R"({{
  "FormatVersion": {},
  "Name": "test",
  "Source": "test.hlsl",
  "Passes": [
    {{
      "Name": "main",
      "Source": "test.hlsl",
      "Stages": [ {{ "Stage": "Vertex", "EntryPoint": "VSMain" }} ],
      "ShaderModel": "SM60",
      "IsOptimize": false,
      "EnableUnbounded": true,
      "VertexInput": {{
        "Buffers": [ {{ "Binding": 0, "ArrayStride": 12, "StepMode": "Vertex" }} ],
        "Attributes": [
          {{ "Semantic": "POSITION", "SemanticIndex": 0, "Format": "FLOAT32X3", "BufferBinding": 0, "Offset": 0 }}
        ]
      }}
    }},
    {{
      "Name": "shadow",
      "Source": "shadow.hlsl",
      "Stages": [ {{ "Stage": "Vertex", "EntryPoint": "ShadowVSMain" }} ],
      "ShaderModel": "SM60",
      "IsOptimize": false,
      "EnableUnbounded": true,
      "VertexInput": {{
        "Buffers": [ {{ "Binding": 0, "ArrayStride": 12, "StepMode": "Vertex" }} ],
        "Attributes": [
          {{ "Semantic": "POSITION", "SemanticIndex": 0, "Format": "FLOAT32X3", "BufferBinding": 0, "Offset": 0 }}
        ]
      }}
    }}
  ]
}})",
        kShaderAssetFormatVersion);
}

struct MultiSourceFixture {
    ShaderWorkspace Workspace;
    ShaderAssetDesc Asset;

    MultiSourceFixture() {
        Workspace.WriteSource("test.hlsl", kSimpleHlsl);
        Workspace.WriteSource("shadow.hlsl", kShadowHlsl);
        Workspace.WriteManifest(MakeMultiSourceManifest());
        ShaderAssetDiagnostic diag;
        auto asset = ParseShaderAssetDesc(MakeMultiSourceManifest(), diag);
        if (asset.has_value()) {
            Asset = std::move(asset.value());
        } else {
            ADD_FAILURE() << "manifest parse failed: " << diag.Message;
        }
    }

    ShaderResolveConfig Config(ShaderArtifactStaleness staleness, bool allowJit) const {
        return ShaderResolveConfig{
            .ShaderRoot = Workspace.Root(),
            .ManifestPath = Workspace.ManifestPath(),
            .Staleness = staleness,
            .AllowJit = allowJit};
    }

    ShaderCookOptions Cook() const {
        return ShaderCookOptions{
            .ShaderRoot = Workspace.Root(),
            .ManifestPath = Workspace.ManifestPath(),
            .Categories = {render::ShaderBlobCategory::DXIL},
            .ValidateReflection = true,
            .Incremental = true};
    }
};

/// 与 Fixture 同构, 但 manifest 带 keyword 组与烘焙声明。
struct VariantFixture {
    ShaderWorkspace Workspace;
    ShaderAssetDesc Asset;

    VariantFixture() {
        Workspace.WriteSource("test.hlsl", kVariantHlsl);
        Workspace.WriteManifest(MakeVariantManifest());
        ShaderAssetDiagnostic diag;
        auto asset = ParseShaderAssetDesc(MakeVariantManifest(), diag);
        if (asset.has_value()) {
            Asset = std::move(asset.value());
        } else {
            ADD_FAILURE() << "manifest parse failed: " << diag.Message;
        }
    }

    ShaderResolveConfig Config(ShaderArtifactStaleness staleness, bool allowJit) const {
        return ShaderResolveConfig{
            .ShaderRoot = Workspace.Root(),
            .ManifestPath = Workspace.ManifestPath(),
            .Staleness = staleness,
            .AllowJit = allowJit};
    }

    const ShaderPassDesc& Pass() const { return Asset.Passes.front(); }

    ShaderCookOptions Cook() const {
        return ShaderCookOptions{
            .ShaderRoot = Workspace.Root(),
            .ManifestPath = Workspace.ManifestPath(),
            .Categories = {render::ShaderBlobCategory::DXIL},
            .ValidateReflection = true,
            .Incremental = true};
    }

    ShaderVariantDomain Domain() const {
        ShaderAssetDiagnostic diag;
        auto domain = ShaderVariantDomain::Build(Asset, Pass(), diag);
        if (!domain.has_value()) {
            ADD_FAILURE() << "domain build failed: " << diag.Message;
            return ShaderVariantDomain{};
        }
        return std::move(domain.value());
    }
};

}  // namespace

TEST(ShaderResolverTest, JitProducesDxil) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, true), dxc.get()};
    ASSERT_TRUE(resolver.CanJit());

    ShaderAssetDiagnostic diag;
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    ASSERT_TRUE(bytecode.has_value()) << diag.Message;
    EXPECT_EQ(bytecode->Source, ShaderBytecodeSource::Jit);
    EXPECT_EQ(bytecode->Category, render::ShaderBlobCategory::DXIL);
    EXPECT_EQ(bytecode->Stage, render::ShaderStage::Vertex);
    EXPECT_FALSE(bytecode->Data.empty());
}

TEST(ShaderResolverTest, JitProducesSpirvWithFourByteAlignment) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, true), dxc.get()};
    ShaderAssetDiagnostic diag;
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::SPIRV,
        {},
        diag);
    ASSERT_TRUE(bytecode.has_value()) << diag.Message;
    EXPECT_EQ(bytecode->Category, render::ShaderBlobCategory::SPIRV);
    // vkCreateShaderModule 会把指针 bit_cast 成 const uint32_t*。
    EXPECT_EQ(bytecode->Data.size() % 4u, 0u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(bytecode->Data.data()) % 4u, 0u);
}

TEST(ShaderResolverTest, DescriptorIsReadyForCreateShader) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, true), dxc.get()};
    ShaderAssetDiagnostic diag;
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Pixel,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    ASSERT_TRUE(bytecode.has_value()) << diag.Message;
    // 这里只断言 ShaderBytecode 自身已就绪; 打包成 render::ShaderDescriptor 的
    // MakeShaderDescriptor 属 runtime 层, 其用例在 test_shader_layout_binding.cpp。
    EXPECT_FALSE(bytecode->Data.empty());
    EXPECT_EQ(bytecode->Category, render::ShaderBlobCategory::DXIL);
    EXPECT_TRUE(bytecode->Stage == render::ShaderStage::Pixel);
}

TEST(ShaderResolverTest, CookWritesIndexAndBlobs) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderCookResult cook = CookShaderAsset(
        *dxc,
        fixture.Asset,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL}));
    ASSERT_TRUE(cook.Succeeded())
        << (cook.Diagnostics.empty() ? string{} : cook.Diagnostics.front().ToString());

    // 两个 stage 各一份。
    EXPECT_EQ(cook.Stats.Compiled, 2u);
    EXPECT_EQ(cook.Index.Entries.size(), 2u);
    EXPECT_TRUE(std::filesystem::exists(fixture.Workspace.ArtifactDir() / "index.json"));
    for (const ShaderArtifactEntry& entry : cook.Index.Entries) {
        EXPECT_TRUE(std::filesystem::exists(
            fixture.Workspace.ArtifactDir() / std::filesystem::path{entry.BlobPath}));
    }
}

TEST(ShaderResolverTest, CookedArtifactIsPreferredOverJit) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderCookResult cook = CookShaderAsset(
        *dxc,
        fixture.Asset,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL}));
    ASSERT_TRUE(cook.Succeeded())
        << (cook.Diagnostics.empty() ? string{} : cook.Diagnostics.front().ToString());

    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, true), dxc.get()};
    ShaderAssetDiagnostic diag;
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    ASSERT_TRUE(bytecode.has_value()) << diag.Message;
    EXPECT_EQ(bytecode->Source, ShaderBytecodeSource::Artifact);
}

TEST(ShaderResolverTest, ArtifactBytecodeMatchesJitBytecode) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    // 先 JIT 拿一份基准。
    ShaderResolver jitOnly{fixture.Config(ShaderArtifactStaleness::Strict, true), dxc.get()};
    ShaderAssetDiagnostic diag;
    auto viaJit = jitOnly.Resolve(
        fixture.Pass(),
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    ASSERT_TRUE(viaJit.has_value()) << diag.Message;

    ShaderCookResult cook = CookShaderAsset(
        *dxc,
        fixture.Asset,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL}));
    ASSERT_TRUE(cook.Succeeded());

    ShaderResolver withArtifacts{fixture.Config(ShaderArtifactStaleness::Strict, true), dxc.get()};
    auto viaArtifact = withArtifacts.Resolve(
        fixture.Pass(),
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    ASSERT_TRUE(viaArtifact.has_value()) << diag.Message;
    EXPECT_EQ(viaArtifact->Source, ShaderBytecodeSource::Artifact);
    // AOT 与 JIT 必须编出完全相同的字节码, 否则 key 的语义就是错的。
    EXPECT_EQ(viaArtifact->Data, viaJit->Data);
    EXPECT_EQ(viaArtifact->Key, viaJit->Key);
}

TEST(ShaderResolverTest, StrictModeFallsBackToJitAfterSourceEdit) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderCookResult cook = CookShaderAsset(
        *dxc,
        fixture.Asset,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL}));
    ASSERT_TRUE(cook.Succeeded());

    // 改源码: Strict 下产物应立即被判为过期。
    fixture.Workspace.WriteSource(
        "test.hlsl",
        string{kSimpleHlsl} + "\nfloat4 Extra() { return 0; }\n");

    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, true), dxc.get()};
    ShaderAssetDiagnostic diag;
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    ASSERT_TRUE(bytecode.has_value()) << diag.Message;
    EXPECT_EQ(bytecode->Source, ShaderBytecodeSource::Jit);
}

TEST(ShaderResolverTest, LenientModeAcceptsStaleArtifact) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderCookResult cook = CookShaderAsset(
        *dxc,
        fixture.Asset,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL}));
    ASSERT_TRUE(cook.Succeeded());

    fixture.Workspace.WriteSource(
        "test.hlsl",
        string{kSimpleHlsl} + "\nfloat4 Extra() { return 0; }\n");

    // Lenient: 发布包里没有 DXC 可回退, 源码微调不应让整包 shader 失效。
    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Lenient, false), nullptr};
    ShaderAssetDiagnostic diag;
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    ASSERT_TRUE(bytecode.has_value()) << diag.Message;
    EXPECT_EQ(bytecode->Source, ShaderBytecodeSource::Artifact);
}

TEST(ShaderResolverTest, LenientModeWorksWithoutSourceFiles) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderCookResult cook = CookShaderAsset(
        *dxc,
        fixture.Asset,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL}));
    ASSERT_TRUE(cook.Succeeded());

    // 模拟发布包: 删掉 HLSL 源, 只留产物。
    std::error_code error;
    std::filesystem::remove(fixture.Workspace.Root() / "test.hlsl", error);

    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Lenient, false), nullptr};
    ShaderAssetDiagnostic diag;
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    ASSERT_TRUE(bytecode.has_value()) << diag.Message;
    EXPECT_EQ(bytecode->Source, ShaderBytecodeSource::Artifact);
}

TEST(ShaderResolverTest, ArtifactMissForOtherCategoryFallsBackToJit) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    // 只烘 DXIL, 然后请求 SPIRV。
    ShaderCookResult cook = CookShaderAsset(
        *dxc,
        fixture.Asset,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL}));
    ASSERT_TRUE(cook.Succeeded());

    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, true), dxc.get()};
    ShaderAssetDiagnostic diag;
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::SPIRV,
        {},
        diag);
    ASSERT_TRUE(bytecode.has_value()) << diag.Message;
    EXPECT_EQ(bytecode->Source, ShaderBytecodeSource::Jit);
}

TEST(ShaderResolverTest, CookBothCategoriesProducesSeparateBlobs) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderCookResult cook = CookShaderAsset(
        *dxc,
        fixture.Asset,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL, render::ShaderBlobCategory::SPIRV}));
    ASSERT_TRUE(cook.Succeeded())
        << (cook.Diagnostics.empty() ? string{} : cook.Diagnostics.front().ToString());
    EXPECT_EQ(cook.Index.Entries.size(), 4u);
    // 按 target 分目录, 使"只发布 DXIL"退化为删一个目录。
    EXPECT_TRUE(std::filesystem::is_directory(fixture.Workspace.ArtifactDir() / "dxil"));
    EXPECT_TRUE(std::filesystem::is_directory(fixture.Workspace.ArtifactDir() / "spirv"));

    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, false), nullptr};
    ShaderAssetDiagnostic diag;
    for (render::ShaderBlobCategory category :
         {render::ShaderBlobCategory::DXIL, render::ShaderBlobCategory::SPIRV}) {
        auto bytecode = resolver.Resolve(
            fixture.Pass(),
            render::ShaderStage::Vertex,
            category,
            {},
            diag);
        ASSERT_TRUE(bytecode.has_value()) << diag.Message;
        EXPECT_EQ(bytecode->Source, ShaderBytecodeSource::Artifact);
        EXPECT_EQ(bytecode->Category, category);
    }
}

TEST(ShaderResolverTest, IncrementalCookReusesExistingBlobs) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    const ShaderCookOptions options = CookOptions(fixture, {render::ShaderBlobCategory::DXIL});
    ShaderCookResult first = CookShaderAsset(*dxc, fixture.Asset, options);
    ASSERT_TRUE(first.Succeeded());
    EXPECT_EQ(first.Stats.Compiled, 2u);
    EXPECT_EQ(first.Stats.Reused, 0u);

    ShaderCookResult second = CookShaderAsset(*dxc, fixture.Asset, options);
    ASSERT_TRUE(second.Succeeded());
    EXPECT_EQ(second.Stats.Compiled, 0u);
    EXPECT_EQ(second.Stats.Reused, 2u);
    EXPECT_EQ(second.Index.Entries.size(), 2u);
}

TEST(ShaderResolverTest, CookFailsWhenManifestContradictsReflection) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    // HLSL 用的是 POSITION, manifest 改成 NORMAL 后反射核对应当失败。
    ShaderAssetDesc broken = fixture.Asset;
    ASSERT_TRUE(broken.Passes.front().VertexInput.has_value());
    broken.Passes.front().VertexInput->Attributes.front().Semantic = "NORMAL";

    ShaderCookResult cook = CookShaderAsset(
        *dxc,
        broken,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL}));
    EXPECT_FALSE(cook.Succeeded());
    ASSERT_FALSE(cook.Diagnostics.empty());
}

TEST(ShaderResolverTest, CookDetectsMissingSource) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderAssetDesc missing = fixture.Asset;
    missing.Passes.front().Source = "absent.hlsl";
    ShaderCookResult cook = CookShaderAsset(
        *dxc,
        missing,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL}));
    EXPECT_FALSE(cook.Succeeded());
}

TEST(ShaderResolverTest, CookRejectsEmptyCategoryList) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderCookResult cook = CookShaderAsset(*dxc, fixture.Asset, CookOptions(fixture, {}));
    EXPECT_FALSE(cook.Succeeded());
}

TEST(ShaderResolverTest, CookFromManifestFileMatchesInMemory) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderCookResult cook = CookShaderAssetFile(
        *dxc,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL}));
    ASSERT_TRUE(cook.Succeeded())
        << (cook.Diagnostics.empty() ? string{} : cook.Diagnostics.front().ToString());
    EXPECT_EQ(cook.Index.AssetName, "test");
    EXPECT_EQ(cook.Index.Entries.size(), 2u);
}

TEST(ShaderResolverTest, CorruptedBlobFallsBackToJit) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderCookResult cook = CookShaderAsset(
        *dxc,
        fixture.Asset,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL}));
    ASSERT_TRUE(cook.Succeeded());

    // 破坏其中一个 blob, index 仍然指向它。
    const std::filesystem::path blob =
        fixture.Workspace.ArtifactDir() /
        std::filesystem::path{cook.Index.Entries.front().BlobPath};
    ASSERT_TRUE(WriteTextFile(blob, "garbage"));

    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, true), dxc.get()};
    ShaderAssetDiagnostic diag;
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        cook.Index.Entries.front().Stage,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    ASSERT_TRUE(bytecode.has_value()) << diag.Message;
    EXPECT_EQ(bytecode->Source, ShaderBytecodeSource::Jit);
}

TEST(ShaderResolverTest, CorruptedBlobWithoutJitFails) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderCookResult cook = CookShaderAsset(
        *dxc,
        fixture.Asset,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL}));
    ASSERT_TRUE(cook.Succeeded());

    const std::filesystem::path blob =
        fixture.Workspace.ArtifactDir() /
        std::filesystem::path{cook.Index.Entries.front().BlobPath};
    ASSERT_TRUE(WriteTextFile(blob, "garbage"));

    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, false), nullptr};
    ShaderAssetDiagnostic diag;
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        cook.Index.Entries.front().Stage,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    EXPECT_FALSE(bytecode.has_value());
}

TEST(ShaderResolverTest, CookBakesEveryDeclaredVariant) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    VariantFixture fixture;
    ShaderCookResult cook = CookShaderAsset(*dxc, fixture.Asset, fixture.Cook());
    ASSERT_TRUE(cook.Succeeded())
        << (cook.Diagnostics.empty() ? string{} : cook.Diagnostics.front().ToString());

    // 两个变体 (关 / 开) x 两个 stage = 4 次尝试, 但 VS 与 keyword 无关, 两个变体
    // 的 VS 投影到同一个 key, 故落地 3 条: VS x1 + PS x2。
    EXPECT_EQ(cook.Index.Entries.size(), 3u);
    EXPECT_EQ(cook.Stats.Compiled, 3u);

    size_t vertex = 0;
    size_t pixelWithKeyword = 0;
    size_t pixelWithout = 0;
    for (const ShaderArtifactEntry& entry : cook.Index.Entries) {
        if (entry.Stage == render::ShaderStage::Vertex) {
            ++vertex;
            // VS 不受该组影响, 投影后必须不带 keyword, 否则去重失效。
            EXPECT_TRUE(entry.Keywords.empty());
        } else if (std::ranges::find(entry.Keywords, "_ALPHATEST_ON") != entry.Keywords.end()) {
            ++pixelWithKeyword;
        } else {
            ++pixelWithout;
        }
    }
    EXPECT_EQ(vertex, 1u);
    EXPECT_EQ(pixelWithKeyword, 1u);
    EXPECT_EQ(pixelWithout, 1u);
}

TEST(ShaderResolverTest, VertexBlobSharedAcrossPixelOnlyVariants) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    VariantFixture fixture;
    ShaderCookResult cook = CookShaderAsset(*dxc, fixture.Asset, fixture.Cook());
    ASSERT_TRUE(cook.Succeeded())
        << (cook.Diagnostics.empty() ? string{} : cook.Diagnostics.front().ToString());
    // 第二个变体的 VS 命中了第一个变体已写下的 key。这条断言就是 stage 投影
    // 归一化的意义所在: 少一次编译, 少一份 blob。
    EXPECT_EQ(cook.Stats.Deduplicated, 1u);
}

TEST(ShaderResolverTest, ResolveHitsBakedVariant) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    VariantFixture fixture;
    ShaderCookResult cook = CookShaderAsset(*dxc, fixture.Asset, fixture.Cook());
    ASSERT_TRUE(cook.Succeeded());

    const ShaderVariantDomain domain = fixture.Domain();
    const std::array<std::string_view, 1> keywords{"_ALPHATEST_ON"};
    ShaderAssetDiagnostic diag;
    auto variant = domain.Resolve(keywords, diag);
    ASSERT_TRUE(variant.has_value()) << diag.Message;

    // 运行时路径: 变体 -> 投影 -> 宏 -> Resolve。
    const vector<string> defines =
        domain.CollectDefines(variant.value(), render::ShaderStage::Pixel);
    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, false), nullptr};
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Pixel,
        render::ShaderBlobCategory::DXIL,
        defines,
        diag);
    ASSERT_TRUE(bytecode.has_value()) << diag.Message;
    EXPECT_EQ(bytecode->Source, ShaderBytecodeSource::Artifact);

    // 默认变体 (keyword 全关) 也已烘焙, 且与上面是不同的 blob。
    const vector<string> defaultDefines =
        domain.CollectDefines(domain.DefaultVariant(), render::ShaderStage::Pixel);
    auto defaultBytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Pixel,
        render::ShaderBlobCategory::DXIL,
        defaultDefines,
        diag);
    ASSERT_TRUE(defaultBytecode.has_value()) << diag.Message;
    EXPECT_EQ(defaultBytecode->Source, ShaderBytecodeSource::Artifact);
    EXPECT_NE(defaultBytecode->Key, bytecode->Key);
}

TEST(ShaderResolverTest, ResolveJitsUnbakedVariant) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    VariantFixture fixture;
    // 去掉烘焙声明: 只有默认变体落地。
    ShaderAssetDesc narrowed = fixture.Asset;
    narrowed.BakeVariants = ShaderBakeSetDesc{};
    ShaderCookResult cook = CookShaderAsset(*dxc, narrowed, fixture.Cook());
    ASSERT_TRUE(cook.Succeeded())
        << (cook.Diagnostics.empty() ? string{} : cook.Diagnostics.front().ToString());
    EXPECT_EQ(cook.Index.Entries.size(), 2u);

    const ShaderVariantDomain domain = fixture.Domain();
    const std::array<std::string_view, 1> keywords{"_ALPHATEST_ON"};
    ShaderAssetDiagnostic diag;
    auto variant = domain.Resolve(keywords, diag);
    ASSERT_TRUE(variant.has_value()) << diag.Message;
    const vector<string> defines =
        domain.CollectDefines(variant.value(), render::ShaderStage::Pixel);

    // 开发构建: 没烘到的变体由 JIT 兜底, 作者不必先跑一遍 cook。
    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, true), dxc.get()};
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Pixel,
        render::ShaderBlobCategory::DXIL,
        defines,
        diag);
    ASSERT_TRUE(bytecode.has_value()) << diag.Message;
    EXPECT_EQ(bytecode->Source, ShaderBytecodeSource::Jit);
}

TEST(ShaderResolverTest, ResolveFailsForUnbakedVariantWhenJitDisabled) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    VariantFixture fixture;
    ShaderAssetDesc narrowed = fixture.Asset;
    narrowed.BakeVariants = ShaderBakeSetDesc{};
    ShaderCookResult cook = CookShaderAsset(*dxc, narrowed, fixture.Cook());
    ASSERT_TRUE(cook.Succeeded());

    const ShaderVariantDomain domain = fixture.Domain();
    const std::array<std::string_view, 1> keywords{"_ALPHATEST_ON"};
    ShaderAssetDiagnostic diag;
    auto variant = domain.Resolve(keywords, diag);
    ASSERT_TRUE(variant.has_value()) << diag.Message;
    const vector<string> defines =
        domain.CollectDefines(variant.value(), render::ShaderStage::Pixel);

    // 发布包: 请求未烘焙的变体必须显式失败, 不能静默降级成别的变体。
    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, false), nullptr};
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Pixel,
        render::ShaderBlobCategory::DXIL,
        defines,
        diag);
    EXPECT_FALSE(bytecode.has_value());
}

TEST(ShaderResolverTest, MultiSourceAssetResolvesEveryPassFromArtifacts) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    MultiSourceFixture fixture;
    ShaderCookResult cook = CookShaderAsset(*dxc, fixture.Asset, fixture.Cook());
    ASSERT_TRUE(cook.Succeeded())
        << (cook.Diagnostics.empty() ? string{} : cook.Diagnostics.front().ToString());
    // 两个源文件各记一份身份: key 是按各自源文件算的, 合并成一份会让查找永远落空。
    ASSERT_EQ(cook.Index.Sources.size(), 2u);
    EXPECT_TRUE(cook.Index.FindSourceIdentity("test.hlsl").has_value());
    EXPECT_TRUE(cook.Index.FindSourceIdentity("shadow.hlsl").has_value());
    EXPECT_NE(
        cook.Index.FindSourceIdentity("test.hlsl").value(),
        cook.Index.FindSourceIdentity("shadow.hlsl").value());

    // 关 JIT: 两个 pass 都必须命中产物, 否则 Resolve 会失败。
    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, false), nullptr};
    for (const ShaderPassDesc& pass : fixture.Asset.Passes) {
        ShaderAssetDiagnostic diag;
        auto bytecode = resolver.Resolve(
            pass,
            render::ShaderStage::Vertex,
            render::ShaderBlobCategory::DXIL,
            {},
            diag);
        ASSERT_TRUE(bytecode.has_value()) << pass.Name << ": " << diag.Message;
        EXPECT_EQ(bytecode->Source, ShaderBytecodeSource::Artifact);
    }
}

TEST(ShaderResolverTest, MultiSourceAssetResolvesInLenientMode) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    MultiSourceFixture fixture;
    ShaderCookResult cook = CookShaderAsset(*dxc, fixture.Asset, fixture.Cook());
    ASSERT_TRUE(cook.Succeeded());

    // 模拟发布包: 删掉源码, Lenient 用 index 记录的身份算 key。
    std::error_code error;
    std::filesystem::remove(fixture.Workspace.Root() / "test.hlsl", error);
    std::filesystem::remove(fixture.Workspace.Root() / "shadow.hlsl", error);

    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Lenient, false), nullptr};
    for (const ShaderPassDesc& pass : fixture.Asset.Passes) {
        ShaderAssetDiagnostic diag;
        auto bytecode = resolver.Resolve(
            pass,
            render::ShaderStage::Vertex,
            render::ShaderBlobCategory::DXIL,
            {},
            diag);
        ASSERT_TRUE(bytecode.has_value()) << pass.Name << ": " << diag.Message;
        EXPECT_EQ(bytecode->Source, ShaderBytecodeSource::Artifact);
    }
}

TEST(ShaderResolverTest, MultiSourceEditInvalidatesOnlyTheEditedPass) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    MultiSourceFixture fixture;
    ShaderCookResult cook = CookShaderAsset(*dxc, fixture.Asset, fixture.Cook());
    ASSERT_TRUE(cook.Succeeded());

    // 只改 shadow.hlsl: main pass 的产物不该受影响。
    fixture.Workspace.WriteSource(
        "shadow.hlsl",
        string{kShadowHlsl} + "\nfloat4 Extra() { return 0; }\n");

    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, true), dxc.get()};
    ShaderAssetDiagnostic diag;
    auto main = resolver.Resolve(
        fixture.Asset.Passes[0],
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    ASSERT_TRUE(main.has_value()) << diag.Message;
    EXPECT_EQ(main->Source, ShaderBytecodeSource::Artifact);

    auto shadow = resolver.Resolve(
        fixture.Asset.Passes[1],
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    ASSERT_TRUE(shadow.has_value()) << diag.Message;
    EXPECT_EQ(shadow->Source, ShaderBytecodeSource::Jit);
}

TEST(ShaderResolverTest, StrictModeSeesSourceEditsOnALiveResolver) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderCookResult cook = CookShaderAsset(
        *dxc,
        fixture.Asset,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL}));
    ASSERT_TRUE(cook.Succeeded());

    // 同一个 resolver 实例先命中产物, 再看到源码编辑。身份缓存必须按时间戳失效,
    // 否则 Strict 承诺的"改 shader 立刻生效"在长命 resolver (RenderSystem 持有) 上失效。
    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, true), dxc.get()};
    ShaderAssetDiagnostic diag;
    auto before = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    ASSERT_TRUE(before.has_value()) << diag.Message;
    EXPECT_EQ(before->Source, ShaderBytecodeSource::Artifact);

    // 文件系统时间戳粒度可能较粗, 显式后移一次写入时间以保证可观测。
    fixture.Workspace.WriteSource(
        "test.hlsl",
        string{kSimpleHlsl} + "\nfloat4 Extra() { return 0; }\n");
    std::error_code error;
    const std::filesystem::path source = fixture.Workspace.Root() / "test.hlsl";
    std::filesystem::last_write_time(
        source,
        std::filesystem::last_write_time(source, error) + std::chrono::seconds{2},
        error);

    auto after = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        {},
        diag);
    ASSERT_TRUE(after.has_value()) << diag.Message;
    EXPECT_EQ(after->Source, ShaderBytecodeSource::Jit);
}

TEST(ShaderResolverTest, DefinesChangeTheResolvedArtifact) {
    auto dxc = MakeDxc();
    if (dxc == nullptr) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    Fixture fixture;
    ShaderCookResult cook = CookShaderAsset(
        *dxc,
        fixture.Asset,
        CookOptions(fixture, {render::ShaderBlobCategory::DXIL}));
    ASSERT_TRUE(cook.Succeeded());

    ShaderResolver resolver{fixture.Config(ShaderArtifactStaleness::Strict, true), dxc.get()};
    ShaderAssetDiagnostic diag;
    // 烘焙时没有这个宏, 故应未命中并回退 JIT。
    const vector<string> defines{"EXTRA_FEATURE=1"};
    auto bytecode = resolver.Resolve(
        fixture.Pass(),
        render::ShaderStage::Vertex,
        render::ShaderBlobCategory::DXIL,
        defines,
        diag);
    ASSERT_TRUE(bytecode.has_value()) << diag.Message;
    EXPECT_EQ(bytecode->Source, ShaderBytecodeSource::Jit);
}

#endif

constexpr std::string_view kForwardSampleSource = "forward_pipeline/forward_pass.hlsl";

/// 仓库根。环境变量 (ctest 注入) 优先, 缺失时回退到配置期编进来的路径,
/// 使这些用例在直接跑 exe (调试器、手动 --gtest_filter 复现) 时同样可用。
std::filesystem::path GetProjectRoot() {
    const string fromEnv = GetEnv("RADRAY_PROJECT_DIR");
    if (!fromEnv.empty()) {
        return std::filesystem::path{fromEnv};
    }
#if defined(RADRAY_PROJECT_DIR_DEFAULT)
    return std::filesystem::path{RADRAY_PROJECT_DIR_DEFAULT};
#else
    return {};
#endif
}

std::filesystem::path GetForwardSampleManifestPath() {
    return GetProjectRoot() / "shaderlib" / "forward_pipeline" /
           "forward_pass.shader.json";
}

constexpr std::string_view kErrorPassSource = "forward_pipeline/error_pass.hlsl";

std::filesystem::path GetErrorPassManifestPath() {
    return GetProjectRoot() / "shaderlib" / "forward_pipeline" /
           "error_pass.shader.json";
}

class ScopedForwardSampleCookDirectory {
public:
    /// manifestFileName: 拷进临时目录后的文件名。cook 会把产物写到 manifest 旁边,
    /// 故每个用例都需要独立目录, 且文件名要与它拷进来的那份 manifest 对应。
    explicit ScopedForwardSampleCookDirectory(
        std::string_view manifestFileName = "forward_pass.shader.json")
        : _manifestFileName(manifestFileName) {
        static std::atomic<uint32_t> counter{0};
        std::error_code error;
        const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(error);
        if (error) {
            return;
        }
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        _path = tempRoot /
                ("radray_forward_shader_sample_" + std::to_string(timestamp) + "_" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(_path, error);
        if (error) {
            _path.clear();
        }
    }

    ~ScopedForwardSampleCookDirectory() {
        std::error_code error;
        if (!_path.empty()) {
            std::filesystem::remove_all(_path, error);
        }
    }

    ScopedForwardSampleCookDirectory(const ScopedForwardSampleCookDirectory&) = delete;
    ScopedForwardSampleCookDirectory& operator=(const ScopedForwardSampleCookDirectory&) = delete;

    bool IsValid() const noexcept { return !_path.empty(); }
    std::filesystem::path ManifestPath() const { return _path / _manifestFileName; }

private:
    std::filesystem::path _path;
    string _manifestFileName;
};

string JoinForwardSampleDiagnostics(const ShaderCookResult& result) {
    string text;
    for (const ShaderAssetDiagnostic& diagnostic : result.Diagnostics) {
        if (!text.empty()) {
            text += '\n';
        }
        text += diagnostic.ToString();
    }
    return text;
}

void ExpectForwardSampleBindingName(
    const ShaderPassDesc& pass,
    uint32_t group,
    uint32_t bindingIndex,
    std::string_view expectedName) {
    Nullable<const ShaderBindingDesc*> binding = pass.FindBinding(group, bindingIndex);
    ASSERT_TRUE(binding.HasValue()) << "missing binding " << group << ':' << bindingIndex;
    EXPECT_EQ(binding.Unwrap()->Name, expectedName);
}

TEST(ShaderAssetSampleTest, ManifestMatchesForwardPassContract) {
    const std::filesystem::path projectRoot = GetProjectRoot();
    ASSERT_FALSE(projectRoot.empty()) << "the project root is unknown";

    const std::filesystem::path shaderRoot = projectRoot / "shaderlib";
    const std::filesystem::path manifestPath = GetForwardSampleManifestPath();
    EXPECT_TRUE(std::filesystem::is_regular_file(shaderRoot / kForwardSampleSource));
    ASSERT_TRUE(std::filesystem::is_regular_file(manifestPath));

    ShaderAssetDiagnostic diagnostic;
    std::optional<ShaderAssetDesc> asset = LoadShaderAssetDesc(manifestPath, diagnostic);
    ASSERT_TRUE(asset.has_value()) << diagnostic.ToString();
    EXPECT_EQ(asset->Name, "ForwardPrincipled");
    EXPECT_EQ(asset->Source, kForwardSampleSource);
    // 8 组: 5 张贴图 + AlphaMode + 两组阴影。混合与双面不在其中 —— 它们是固定功能
    // 状态 (MaterialRenderState), 不是变体维度。
    ASSERT_EQ(asset->KeywordGroups.size(), 8u);
    ASSERT_EQ(asset->Passes.size(), 1u);

    const ShaderPassDesc& pass = asset->Passes.front();
    EXPECT_EQ(pass.Name, "Forward");
    EXPECT_EQ(pass.GetStageMask(), render::ShaderStages{render::ShaderStage::Graphics});
    EXPECT_EQ(pass.FindEntryPoint(render::ShaderStage::Vertex), "VSMain");
    EXPECT_EQ(pass.FindEntryPoint(render::ShaderStage::Pixel), "PSMain");

    Nullable<const ShaderBindingDesc*> perObject = pass.FindBinding(0, 1);
    ASSERT_TRUE(perObject.HasValue());
    EXPECT_EQ(perObject.Unwrap()->Name, "gPerObject");
    EXPECT_EQ(perObject.Unwrap()->Residency, ShaderBindingResidency::RootDescriptor);
    EXPECT_EQ(perObject.Unwrap()->Stages, render::ShaderStages{render::ShaderStage::Vertex});

    Nullable<const ShaderBindingDesc*> view = pass.FindBinding(1, 0);
    ASSERT_TRUE(view.HasValue());
    EXPECT_EQ(view.Unwrap()->Name, "gView");
    EXPECT_EQ(view.Unwrap()->Residency, ShaderBindingResidency::RootDescriptor);
    EXPECT_EQ(view.Unwrap()->Stages, render::ShaderStages{render::ShaderStage::Graphics});

    ExpectForwardSampleBindingName(pass, 1, 1, "gShadowCube");
    ExpectForwardSampleBindingName(pass, 1, 2, "gShadowArray");
    ExpectForwardSampleBindingName(pass, 1, 3, "gShadowSampler");
    ExpectForwardSampleBindingName(pass, 2, 0, "gMaterial");
    ExpectForwardSampleBindingName(pass, 2, 6, "gSampler");

    ASSERT_TRUE(pass.VertexInput.has_value());
    ASSERT_EQ(pass.VertexInput->Buffers.size(), 1u);
    EXPECT_EQ(pass.VertexInput->Buffers.front().ArrayStride, 48u);
    ASSERT_EQ(pass.VertexInput->Attributes.size(), 4u);
    EXPECT_EQ(pass.VertexInput->Attributes[0].Semantic, "POSITION");
    EXPECT_EQ(pass.VertexInput->Attributes[0].Offset, 0u);
    EXPECT_EQ(pass.VertexInput->Attributes[1].Semantic, "NORMAL");
    EXPECT_EQ(pass.VertexInput->Attributes[1].Offset, 12u);
    EXPECT_EQ(pass.VertexInput->Attributes[2].Semantic, "TEXCOORD");
    EXPECT_EQ(pass.VertexInput->Attributes[2].Offset, 24u);
    EXPECT_EQ(pass.VertexInput->Attributes[3].Semantic, "TANGENT");
    EXPECT_EQ(pass.VertexInput->Attributes[3].Offset, 32u);

    std::optional<ShaderVariantDomain> domain =
        ShaderVariantDomain::Build(asset.value(), pass, diagnostic);
    ASSERT_TRUE(domain.has_value()) << diagnostic.ToString();
    EXPECT_EQ(domain->GroupCount(), 8u);

    std::optional<vector<ShaderVariantKey>> variants = ExpandShaderBakeSet(
        domain.value(),
        GetEffectiveBakeSet(asset.value(), pass),
        true,
        diagnostic);
    ASSERT_TRUE(variants.has_value()) << diagnostic.ToString();
    // 默认变体 (全关) + 声明的 full-feature 组合。
    EXPECT_EQ(variants->size(), 2u);
}

// error_pass 是垂直切片 (端到端 draw 测试) 用的 shader: 顶点只有 POSITION,
// PSMain 返回洋红常量, 无材质绑定。本用例锁住它的 manifest 契约, 使切片测试里的
// buffer 布局与绑定编号有据可依。
TEST(ShaderAssetSampleTest, ManifestMatchesErrorPassContract) {
    const std::filesystem::path projectRoot = GetProjectRoot();
    ASSERT_FALSE(projectRoot.empty()) << "the project root is unknown";

    const std::filesystem::path shaderRoot = projectRoot / "shaderlib";
    const std::filesystem::path manifestPath = GetErrorPassManifestPath();
    EXPECT_TRUE(std::filesystem::is_regular_file(shaderRoot / kErrorPassSource));
    ASSERT_TRUE(std::filesystem::is_regular_file(manifestPath));

    ShaderAssetDiagnostic diagnostic;
    std::optional<ShaderAssetDesc> asset = LoadShaderAssetDesc(manifestPath, diagnostic);
    ASSERT_TRUE(asset.has_value()) << diagnostic.ToString();
    EXPECT_EQ(asset->Name, "ErrorPass");
    EXPECT_EQ(asset->Source, kErrorPassSource);

    // 两组都从 <forward_pipeline/view.hlsli> 继承而来, error_pass 自己没有 pragma。
    // 入口 shader 继承 include 的 keyword 组, 这里正是该规则的最小见证。
    ASSERT_EQ(asset->KeywordGroups.size(), 2u);
    EXPECT_EQ(asset->KeywordGroups[0].Name, "PointShadows");
    EXPECT_EQ(asset->KeywordGroups[1].Name, "DirectionalShadows");

    ASSERT_EQ(asset->Passes.size(), 1u);
    const ShaderPassDesc& pass = asset->Passes.front();
    EXPECT_EQ(pass.Name, "Error");
    EXPECT_EQ(pass.GetStageMask(), render::ShaderStages{render::ShaderStage::Graphics});
    EXPECT_EQ(pass.FindEntryPoint(render::ShaderStage::Vertex), "VSMain");
    EXPECT_EQ(pass.FindEntryPoint(render::ShaderStage::Pixel), "PSMain");

    Nullable<const ShaderBindingDesc*> perObject = pass.FindBinding(0, 1);
    ASSERT_TRUE(perObject.HasValue());
    EXPECT_EQ(perObject.Unwrap()->Name, "gPerObject");
    EXPECT_EQ(perObject.Unwrap()->Stages, render::ShaderStages{render::ShaderStage::Vertex});

    // gView 只标 Vertex: PSMain 返回常量, 从不读它。
    Nullable<const ShaderBindingDesc*> view = pass.FindBinding(1, 0);
    ASSERT_TRUE(view.HasValue());
    EXPECT_EQ(view.Unwrap()->Name, "gView");
    EXPECT_EQ(view.Unwrap()->Stages, render::ShaderStages{render::ShaderStage::Vertex});

    // 阴影绑定虽被 keyword 守护着存在于 view.hlsli, 但 error_pass 从不采样,
    // 故不出现在任何变体的反射里, manifest 也不该声明。
    EXPECT_FALSE(pass.FindBinding(1, 1).HasValue()) << "gShadowCube must not be declared";
    EXPECT_FALSE(pass.FindBinding(1, 2).HasValue()) << "gShadowArray must not be declared";
    EXPECT_FALSE(pass.FindBinding(1, 3).HasValue()) << "gShadowSampler must not be declared";
    // 材质组整个不存在 —— 兜底 pass 不能依赖正在失败的那一环。
    EXPECT_FALSE(pass.FindBinding(2, 0).HasValue()) << "the fallback pass has no material group";

    ASSERT_TRUE(pass.VertexInput.has_value());
    ASSERT_EQ(pass.VertexInput->Buffers.size(), 1u);
    EXPECT_EQ(pass.VertexInput->Buffers.front().ArrayStride, 12u);
    ASSERT_EQ(pass.VertexInput->Attributes.size(), 1u);
    EXPECT_EQ(pass.VertexInput->Attributes[0].Semantic, "POSITION");
    EXPECT_EQ(pass.VertexInput->Attributes[0].SemanticIndex, 0u);
    EXPECT_EQ(pass.VertexInput->Attributes[0].Offset, 0u);

    std::optional<ShaderVariantDomain> domain =
        ShaderVariantDomain::Build(asset.value(), pass, diagnostic);
    ASSERT_TRUE(domain.has_value()) << diagnostic.ToString();
    EXPECT_EQ(domain->GroupCount(), 2u);

    // 未声明 BakeVariants 规则 => 只有默认变体。
    std::optional<vector<ShaderVariantKey>> variants = ExpandShaderBakeSet(
        domain.value(),
        GetEffectiveBakeSet(asset.value(), pass),
        true,
        diagnostic);
    ASSERT_TRUE(variants.has_value()) << diagnostic.ToString();
    EXPECT_EQ(variants->size(), 1u);
}

#if defined(RADRAY_ENABLE_SHADER_JIT)

TEST(ShaderAssetSampleTest, ManifestCooksRealErrorPassShader) {
    auto dxcResult = render::CreateDxc();
    if (!dxcResult.HasValue()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    shared_ptr<render::Dxc> dxc = dxcResult.Release();

    ScopedForwardSampleCookDirectory output{"error_pass.shader.json"};
    ASSERT_TRUE(output.IsValid());

    std::error_code error;
    std::filesystem::copy_file(
        GetErrorPassManifestPath(),
        output.ManifestPath(),
        std::filesystem::copy_options::overwrite_existing,
        error);
    ASSERT_FALSE(error) << error.message();

    vector<render::ShaderBlobCategory> categories{render::ShaderBlobCategory::DXIL};
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
    categories.push_back(render::ShaderBlobCategory::SPIRV);
#endif

    // ValidateReflection: 真正的断言在这里 —— manifest 声明的 ABI 必须与 DXC
    // 反射出来的一致, 否则 cook 失败。
    const ShaderCookOptions options{
        .ShaderRoot = GetProjectRoot() / "shaderlib",
        .ManifestPath = output.ManifestPath(),
        .Categories = categories,
        .ValidateReflection = true,
        .Incremental = false};
    ShaderCookResult cook = CookShaderAssetFile(*dxc, options);
    ASSERT_TRUE(cook.Succeeded()) << JoinForwardSampleDiagnostics(cook);

    // 只有默认变体, 每个 category 落地 1 VS + 1 PS。
    const size_t expectedEntries = categories.size() * 2u;
    EXPECT_EQ(cook.Index.AssetName, "ErrorPass");
    EXPECT_EQ(cook.Index.Entries.size(), expectedEntries);
    EXPECT_EQ(cook.Stats.Compiled, expectedEntries);

    const std::filesystem::path artifactDirectory =
        GetShaderArtifactDirectory(options.ManifestPath);
    ASSERT_TRUE(std::filesystem::is_regular_file(artifactDirectory / "index.json"));
    for (const ShaderArtifactEntry& entry : cook.Index.Entries) {
        EXPECT_EQ(entry.PassName, "Error");
        EXPECT_EQ(entry.Source, kErrorPassSource);
        EXPECT_TRUE(entry.Keywords.empty()) << "the default variant enables nothing";
        EXPECT_GT(entry.BytecodeSize, 0u);
        EXPECT_TRUE(std::filesystem::is_regular_file(
            artifactDirectory / std::filesystem::path{entry.BlobPath}));
    }
}

/// cook 出的产物能被"发布包配置" (AllowJit == false, 无 DXC) 解析。
///
/// 【为何单独一个用例】: 上面的 ManifestCooksRealErrorPassShader 只断言产物写出来了,
/// 没有人读过它。而 ShaderResolver 里 AOT 命中那一整段 (toolchain 比对、按源文件取
/// cook 时身份、算 key、读 blob 自验) 在真实 manifest 上从未跑过 —— 之前的覆盖全在
/// 临时目录里手写的 test.hlsl 上。发布包路径与开发路径的差别恰恰在"没有 JIT 可兜底",
/// 任何一步算错 key 都会从"慢一点"变成"起不来"。
TEST(ShaderAssetSampleTest, CookedErrorPassResolvesWithoutJit) {
    auto dxcResult = render::CreateDxc();
    if (!dxcResult.HasValue()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    shared_ptr<render::Dxc> dxc = dxcResult.Release();

    ScopedForwardSampleCookDirectory output{"error_pass.shader.json"};
    ASSERT_TRUE(output.IsValid());
    std::error_code error;
    std::filesystem::copy_file(
        GetErrorPassManifestPath(),
        output.ManifestPath(),
        std::filesystem::copy_options::overwrite_existing,
        error);
    ASSERT_FALSE(error) << error.message();

    const std::filesystem::path shaderRoot = GetProjectRoot() / "shaderlib";
    vector<render::ShaderBlobCategory> categories{render::ShaderBlobCategory::DXIL};
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
    categories.push_back(render::ShaderBlobCategory::SPIRV);
#endif

    const ShaderCookOptions cookOptions{
        .ShaderRoot = shaderRoot,
        .ManifestPath = output.ManifestPath(),
        .Categories = categories,
        .ValidateReflection = true,
        .Incremental = false};
    ShaderCookResult cook = CookShaderAssetFile(*dxc, cookOptions);
    ASSERT_TRUE(cook.Succeeded()) << JoinForwardSampleDiagnostics(cook);

    ShaderAssetDiagnostic diagnostic;
    std::optional<ShaderAssetDesc> asset = LoadShaderAssetDesc(output.ManifestPath(), diagnostic);
    ASSERT_TRUE(asset.has_value()) << diagnostic.ToString();
    const ShaderPassDesc pass = MakeResolvablePass(asset.value(), asset->Passes.front());
    std::optional<ShaderVariantDomain> domain =
        ShaderVariantDomain::Build(asset.value(), asset->Passes.front(), diagnostic);
    ASSERT_TRUE(domain.has_value()) << diagnostic.ToString();
    const ShaderVariantKey variant = domain->DefaultVariant();

    // dxc 传 nullptr 而不是 dxc.get(): 发布包里根本没有 DXC。给了指针再设
    // AllowJit = false 只测到"我们没去用它", 传 nullptr 才测到"用不了也能起来"。
    ShaderResolver strict{
        ShaderResolveConfig{
            .ShaderRoot = shaderRoot,
            .ManifestPath = output.ManifestPath(),
            .Staleness = ShaderArtifactStaleness::Strict,
            .AllowJit = false},
        nullptr};
    EXPECT_FALSE(strict.CanJit());

    for (const render::ShaderBlobCategory category : categories) {
        for (const render::ShaderStage stage :
             {render::ShaderStage::Vertex, render::ShaderStage::Pixel}) {
            const vector<string> defines = domain->CollectDefines(variant, stage);
            std::optional<ShaderBytecode> bytecode =
                strict.Resolve(pass, stage, category, defines, diagnostic);
            ASSERT_TRUE(bytecode.has_value())
                << fmt::format("category {} stage {}: ", category, stage)
                << diagnostic.ToString();
            EXPECT_EQ(bytecode->Source, ShaderBytecodeSource::Artifact);
            EXPECT_EQ(bytecode->Category, category);
            EXPECT_EQ(bytecode->Stage, stage);
            EXPECT_FALSE(bytecode->Data.empty());
            // key 必须与 cook 时写下的那条对上 —— 这是"运行时纯函数算 key"这条设计
            // 唯一的真实检验点。
            EXPECT_TRUE(cook.Index.Find(bytecode->Key).HasValue());
        }
    }

    // Lenient 是发布包真正会用的策略: 源码可能根本没部署。这里删掉整个 shader root
    // 的可见性 (指向一个不存在的目录) 模拟该情形, index 自称的身份足以算出 key。
    ShaderResolver lenient{
        ShaderResolveConfig{
            .ShaderRoot = shaderRoot / "does_not_exist",
            .ManifestPath = output.ManifestPath(),
            .Staleness = ShaderArtifactStaleness::Lenient,
            .AllowJit = false},
        nullptr};
    const vector<string> vsDefines =
        domain->CollectDefines(variant, render::ShaderStage::Vertex);
    std::optional<ShaderBytecode> lenientVs = lenient.Resolve(
        pass, render::ShaderStage::Vertex, categories.front(), vsDefines, diagnostic);
    ASSERT_TRUE(lenientVs.has_value()) << diagnostic.ToString();
    EXPECT_EQ(lenientVs->Source, ShaderBytecodeSource::Artifact);

    // 同样的配置换 Strict 必须失败: 算不出源码身份且无 JIT 时不能猜。
    ShaderResolver strictWithoutSources{
        ShaderResolveConfig{
            .ShaderRoot = shaderRoot / "does_not_exist",
            .ManifestPath = output.ManifestPath(),
            .Staleness = ShaderArtifactStaleness::Strict,
            .AllowJit = false},
        nullptr};
    EXPECT_FALSE(
        strictWithoutSources
            .Resolve(pass, render::ShaderStage::Vertex, categories.front(), vsDefines, diagnostic)
            .has_value());
}

/// 未烘焙的变体在发布包配置下必须显式失败。
///
/// error_pass 只烘默认变体 (两组阴影 keyword 全关), 故请求 `_POINT_SHADOWS` 是一个
/// 合法但未预编的组合 —— 正是发布包里最容易踩到的那类错误。它必须报错, 而不是静默
/// 退回默认变体: 后者会画出一张"看起来对"但缺特性的图, 无从发现。
TEST(ShaderAssetSampleTest, UnbakedErrorPassVariantFailsWithoutJit) {
    auto dxcResult = render::CreateDxc();
    if (!dxcResult.HasValue()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    shared_ptr<render::Dxc> dxc = dxcResult.Release();

    ScopedForwardSampleCookDirectory output{"error_pass.shader.json"};
    ASSERT_TRUE(output.IsValid());
    std::error_code error;
    std::filesystem::copy_file(
        GetErrorPassManifestPath(),
        output.ManifestPath(),
        std::filesystem::copy_options::overwrite_existing,
        error);
    ASSERT_FALSE(error) << error.message();

    const std::filesystem::path shaderRoot = GetProjectRoot() / "shaderlib";
    const vector<render::ShaderBlobCategory> categories{render::ShaderBlobCategory::DXIL};
    const ShaderCookOptions cookOptions{
        .ShaderRoot = shaderRoot,
        .ManifestPath = output.ManifestPath(),
        .Categories = categories,
        .ValidateReflection = true,
        .Incremental = false};
    ASSERT_TRUE(CookShaderAssetFile(*dxc, cookOptions).Succeeded());

    ShaderAssetDiagnostic diagnostic;
    std::optional<ShaderAssetDesc> asset = LoadShaderAssetDesc(output.ManifestPath(), diagnostic);
    ASSERT_TRUE(asset.has_value()) << diagnostic.ToString();
    const ShaderPassDesc pass = MakeResolvablePass(asset.value(), asset->Passes.front());
    std::optional<ShaderVariantDomain> domain =
        ShaderVariantDomain::Build(asset.value(), asset->Passes.front(), diagnostic);
    ASSERT_TRUE(domain.has_value()) << diagnostic.ToString();

    const std::array<std::string_view, 1> keywords{"_POINT_SHADOWS"};
    std::optional<ShaderVariantKey> variant = domain->Resolve(keywords, diagnostic);
    ASSERT_TRUE(variant.has_value()) << diagnostic.ToString();
    // 该 keyword 组只作用于 Pixel, 故必须查 PS —— VS 的投影会把它归零, 于是 VS 反而
    // 会命中默认变体的 blob。
    const vector<string> defines =
        domain->CollectDefines(variant.value(), render::ShaderStage::Pixel);

    ShaderResolver resolver{
        ShaderResolveConfig{
            .ShaderRoot = shaderRoot,
            .ManifestPath = output.ManifestPath(),
            .Staleness = ShaderArtifactStaleness::Strict,
            .AllowJit = false},
        nullptr};
    EXPECT_FALSE(
        resolver.Resolve(pass, render::ShaderStage::Pixel, categories.front(), defines, diagnostic)
            .has_value());

    // 同一份产物在开发配置下应当由 JIT 兜底 —— 上面的失败来自"没有 JIT", 不是
    // "这个变体本身非法"。
    ShaderResolver developer{
        ShaderResolveConfig{
            .ShaderRoot = shaderRoot,
            .ManifestPath = output.ManifestPath(),
            .Staleness = ShaderArtifactStaleness::Strict,
            .AllowJit = true},
        dxc.get()};
    std::optional<ShaderBytecode> jitted = developer.Resolve(
        pass, render::ShaderStage::Pixel, categories.front(), defines, diagnostic);
    ASSERT_TRUE(jitted.has_value()) << diagnostic.ToString();
    EXPECT_EQ(jitted->Source, ShaderBytecodeSource::Jit);
}

TEST(ShaderAssetSampleTest, ManifestCooksRealForwardShader) {
    auto dxcResult = render::CreateDxc();
    if (!dxcResult.HasValue()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    shared_ptr<render::Dxc> dxc = dxcResult.Release();

    ScopedForwardSampleCookDirectory output;
    ASSERT_TRUE(output.IsValid());

    std::error_code error;
    std::filesystem::copy_file(
        GetForwardSampleManifestPath(),
        output.ManifestPath(),
        std::filesystem::copy_options::overwrite_existing,
        error);
    ASSERT_FALSE(error) << error.message();

    vector<render::ShaderBlobCategory> categories{render::ShaderBlobCategory::DXIL};
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
    categories.push_back(render::ShaderBlobCategory::SPIRV);
#endif

    const ShaderCookOptions options{
        .ShaderRoot = GetProjectRoot() / "shaderlib",
        .ManifestPath = output.ManifestPath(),
        .Categories = categories,
        .ValidateReflection = true,
        .Incremental = false};
    ShaderCookResult cook = CookShaderAssetFile(*dxc, options);
    ASSERT_TRUE(cook.Succeeded()) << JoinForwardSampleDiagnostics(cook);

    // 每个 category: 2 变体 × 2 stage = 4 次请求, 但所有 keyword 组都是 pixel-only,
    // 故两个变体投影到同一份 VS, 落地 1 VS + 2 PS = 3 条 entry, 去重 1 次。
    const size_t expectedEntries = categories.size() * 3u;
    EXPECT_EQ(cook.Index.AssetName, "ForwardPrincipled");
    EXPECT_EQ(cook.Index.Entries.size(), expectedEntries);
    EXPECT_EQ(cook.Stats.Compiled, expectedEntries);
    EXPECT_EQ(cook.Stats.Reused, 0u);
    EXPECT_EQ(cook.Stats.Deduplicated, categories.size());

    const std::filesystem::path artifactDirectory =
        GetShaderArtifactDirectory(options.ManifestPath);
    ASSERT_TRUE(std::filesystem::is_regular_file(artifactDirectory / "index.json"));

    for (render::ShaderBlobCategory category : categories) {
        size_t vertexCount = 0;
        size_t pixelCount = 0;
        for (const ShaderArtifactEntry& entry : cook.Index.Entries) {
            if (entry.Category != category) {
                continue;
            }
            EXPECT_EQ(entry.PassName, "Forward");
            EXPECT_EQ(entry.Source, kForwardSampleSource);
            EXPECT_FALSE(entry.Key.IsZero());
            EXPECT_FALSE(entry.BytecodeHash.IsZero());
            EXPECT_GT(entry.BytecodeSize, 0u);
            EXPECT_TRUE(std::filesystem::is_regular_file(
                artifactDirectory / std::filesystem::path{entry.BlobPath}));
            if (entry.Stage == render::ShaderStage::Vertex) {
                ++vertexCount;
                EXPECT_TRUE(entry.Keywords.empty());
            } else if (entry.Stage == render::ShaderStage::Pixel) {
                ++pixelCount;
            }
        }
        EXPECT_EQ(vertexCount, 1u);
        EXPECT_EQ(pixelCount, 2u);
    }

    ShaderAssetDiagnostic diagnostic;
    std::optional<ShaderArtifactIndex> persisted =
        LoadShaderArtifactIndex(artifactDirectory / "index.json", diagnostic);
    ASSERT_TRUE(persisted.has_value()) << diagnostic.ToString();
    EXPECT_EQ(persisted->Entries.size(), expectedEntries);
}

#endif

}  // namespace
}  // namespace radray
