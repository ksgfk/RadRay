#include <array>
#include <cstring>

#include <gtest/gtest.h>

#include <radray/runtime/material_asset.h>
#include <radray/runtime/render_framework/forward_pipeline.h>

namespace radray {
namespace {

shader::HlslShaderDesc MakeMaterialReflection() {
    shader::HlslShaderDesc result;
    result.ConstantBuffers.emplace_back(shader::HlslShaderBufferDesc{
        .Name = "gMaterial",
        .Variables = {0, 1},
        .Type = shader::HlslCBufferType::CBUFFER,
        .Size = 32});
    result.Variables.emplace_back(shader::HlslShaderVariableDesc{
        .Name = "BaseColor",
        .StartOffset = 0,
        .Size = 16});
    result.Variables.emplace_back(shader::HlslShaderVariableDesc{
        .Name = "Roughness",
        .StartOffset = 16,
        .Size = 4});
    result.BoundResources.emplace_back(shader::HlslInputBindDesc{
        .Name = "gMaterial",
        .Type = shader::HlslShaderInputType::CBUFFER,
        .BindPoint = 0,
        .BindCount = 1,
        .Space = ForwardPipeline::kMaterialBindingGroup});
    result.BoundResources.emplace_back(shader::HlslInputBindDesc{
        .Name = "gBaseColorMap",
        .Type = shader::HlslShaderInputType::TEXTURE,
        .BindPoint = 1,
        .BindCount = 1,
        .Dimension = shader::HlslSRVDimension::TEXTURE2D,
        .Space = ForwardPipeline::kMaterialBindingGroup});
    result.BoundResources.emplace_back(shader::HlslInputBindDesc{
        .Name = "gSampler",
        .Type = shader::HlslShaderInputType::SAMPLER,
        .BindPoint = 2,
        .BindCount = 1,
        .Space = ForwardPipeline::kMaterialBindingGroup});
    return result;
}

shader::CompiledShaderStage MakeStage(vector<string> defines, byte marker) {
    shader::CompiledShaderStage stage;
    stage.Target = shader::ShaderTarget::DXIL;
    stage.Category = shader::ShaderBlobCategory::DXIL;
    stage.PassIndex = 0;
    stage.Stage = shader::ShaderStage::Vertex;
    stage.Defines = std::move(defines);
    stage.EntryPoint = "VSMain";
    stage.Bytecode = {marker};
    shader::HlslShaderDesc reflection = MakeMaterialReflection();
    stage.Reflection = reflection;
    stage.ReflectionPayload = shader::SerializeHlslShaderDesc(reflection).value();
    stage.BinaryHash = shader::HashShaderBytes(stage.Bytecode);
    stage.InterfaceHash = shader::HashShaderBytes(std::as_bytes(std::span{
        stage.ReflectionPayload.data(), stage.ReflectionPayload.size()}));
    return stage;
}

shader::ShaderBinary MakeShaderBinary() {
    shader::ShaderBinary result;
    result.Asset.AssetId = Guid::NewGuid();
    shader::ShaderPassDesc pass;
    pass.Name = "Forward";
    pass.SourcePath = "forward_pipeline/forward_pass.hlsl";
    pass.KeywordGroups = {
        shader::ShaderKeywordGroupDesc{
            .Alternatives = {"", "_ALPHATEST_ON=1", "_ALPHABLEND_ON=1"},
            .Scope = shader::ShaderKeywordScope::Local,
            .Stages = shader::ShaderStage::Vertex},
        shader::ShaderKeywordGroupDesc{
            .Alternatives = {"", "_POINT_SHADOWS=1"},
            .Scope = shader::ShaderKeywordScope::Global,
            .Stages = shader::ShaderStage::Vertex}};
    pass.Variants = {
        shader::ShaderVariantDesc{},
        shader::ShaderVariantDesc{{"_ALPHATEST_ON=1"}},
        shader::ShaderVariantDesc{{"_ALPHABLEND_ON=1"}},
        shader::ShaderVariantDesc{{"_POINT_SHADOWS=1"}},
        shader::ShaderVariantDesc{{"_ALPHABLEND_ON=1", "_POINT_SHADOWS=1"}}};
    std::get<shader::ShaderGraphicsPassDesc>(pass.Program).VertexEntry = "VSMain";
    result.Asset.Passes.emplace_back(std::move(pass));
    result.Stages = {
        MakeStage({}, byte{0x01}),
        MakeStage({"_ALPHATEST_ON=1"}, byte{0x02}),
        MakeStage({"_ALPHABLEND_ON=1"}, byte{0x03}),
        MakeStage({"_POINT_SHADOWS=1"}, byte{0x04}),
        MakeStage({"_ALPHABLEND_ON=1", "_POINT_SHADOWS=1"}, byte{0x05})};
    return result;
}

TEST(MaterialParameterStorageTest, PacksConstantsByReflectedOffsets) {
    shader::ShaderBinary binary = MakeShaderBinary();
    auto layout = BuildMaterialLayout(
        binary.Stages,
        ForwardPipeline::kMaterialBindingGroup);
    ASSERT_TRUE(layout.has_value());

    MaterialParameterStorage storage;
    ASSERT_TRUE(storage.Reset(*layout));
    const Eigen::Vector4f color{0.2f, 0.4f, 0.6f, 1.0f};
    ASSERT_TRUE(storage.SetVector("gMaterial.BaseColor", color));
    ASSERT_TRUE(storage.SetFloat("Roughness", 0.35f));
    EXPECT_FALSE(storage.SetFloat("BaseColor", 1.0f));
    EXPECT_FALSE(storage.SetFloat("Missing", 1.0f));

    ASSERT_EQ(storage.ConstantBindings().size(), 1u);
    const vector<byte>& bytes = storage.ConstantBindings()[0].Data;
    Eigen::Vector4f storedColor;
    float storedRoughness = 0.0f;
    std::memcpy(storedColor.data(), bytes.data(), sizeof(float) * 4);
    std::memcpy(&storedRoughness, bytes.data() + 16, sizeof(float));
    EXPECT_TRUE(storedColor.isApprox(color));
    EXPECT_FLOAT_EQ(storedRoughness, 0.35f);
}

TEST(MaterialAssetTest, SeparatesLocalAndPipelineKeywords) {
    shader::ShaderBinary binary = MakeShaderBinary();
    ASSERT_TRUE(binary.IsValid());
    AssetManager assets;
    StreamingAssetRef<ShaderAsset> shaderRef =
        assets.AddReady(Guid::NewGuid(), make_unique<ShaderAsset>(std::move(binary)));
    ASSERT_TRUE(shaderRef.IsReady());

    MaterialAsset material{shaderRef, ForwardPipeline::kMaterialBindingGroup};
    ASSERT_TRUE(material.IsReady());
    EXPECT_EQ(material.GetBindingGroup(), ForwardPipeline::kMaterialBindingGroup);
    EXPECT_FALSE(material.EnableLocalKeyword("_POINT_SHADOWS=1"));
    ASSERT_TRUE(material.EnableLocalKeyword("_ALPHATEST_ON=1"));
    ASSERT_TRUE(material.EnableLocalKeyword("_ALPHABLEND_ON=1"));
    EXPECT_FALSE(material.IsLocalKeywordEnabled("_ALPHATEST_ON=1"));
    EXPECT_TRUE(material.IsLocalKeywordEnabled("_ALPHABLEND_ON=1"));

    const std::array<std::string_view, 1> globals{"_POINT_SHADOWS=1"};
    auto defines = material.ResolveVariantDefines(0, globals);
    ASSERT_TRUE(defines.has_value());
    EXPECT_EQ(*defines, (vector<string>{"_ALPHABLEND_ON=1", "_POINT_SHADOWS=1"}));

    const std::array<std::string_view, 1> invalidOwner{"_ALPHATEST_ON=1"};
    EXPECT_FALSE(material.ResolveVariantDefines(0, invalidOwner).has_value());
    EXPECT_TRUE(material.SetVector("BaseColor", Eigen::Vector4f::Ones()));
    EXPECT_TRUE(material.SetFloat("gMaterial.Roughness", 0.5f));
    EXPECT_FALSE(material.SetFloat("gView.Exposure", 1.0f));
}

TEST(MaterialAssetTest, BindingGroupIsConfiguredAtRuntime) {
    shader::ShaderBinary binary = MakeShaderBinary();
    ASSERT_TRUE(binary.IsValid());
    AssetManager assets;
    StreamingAssetRef<ShaderAsset> shaderRef =
        assets.AddReady(Guid::NewGuid(), make_unique<ShaderAsset>(std::move(binary)));
    ASSERT_TRUE(shaderRef.IsReady());

    MaterialAsset material;
    EXPECT_FALSE(material.IsReady());
    ASSERT_TRUE(material.SetShader(shaderRef, ForwardPipeline::kMaterialBindingGroup));
    EXPECT_TRUE(material.IsReady());
    EXPECT_EQ(material.GetBindingGroup(), ForwardPipeline::kMaterialBindingGroup);
}

}  // namespace
}  // namespace radray
