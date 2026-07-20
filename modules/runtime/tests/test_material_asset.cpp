#include <array>

#include <gtest/gtest.h>

#include <radray/runtime/material_asset.h>
#include <radray/runtime/shader_asset.h>

namespace radray {
namespace {

shader::ShaderBindingDesc MakeSampler(
    uint32_t group,
    uint32_t binding,
    std::string_view name) {
    (void)group;
    return shader::ShaderBindingDesc{
        .Name = string{name},
        .BindingIndex = binding,
        .Kind = shader::ShaderBindingKind::Sampler,
        .Access = shader::ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Stages = shader::ShaderStage::Vertex};
}

shader::ShaderBindingDesc MakeTexture(
    uint32_t binding,
    std::string_view name) {
    return shader::ShaderBindingDesc{
        .Name = string{name},
        .BindingIndex = binding,
        .Kind = shader::ShaderBindingKind::SampledTexture,
        .Access = shader::ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Stages = shader::ShaderStage::Pixel,
        .Texture = shader::ShaderTextureInterfaceDesc{
            .Dimension = shader::ShaderTextureDimension::Dim2D,
            .SampleType = shader::ShaderSampleType::Float}};
}

shader::HlslShaderDesc MakeReflection() {
    shader::HlslShaderDesc result;
    result.BoundResources = {
        shader::HlslInputBindDesc{
            .Name = "PipelineSampler",
            .Type = shader::HlslShaderInputType::SAMPLER,
            .BindPoint = 0,
            .BindCount = 1,
            .Space = 0},
        shader::HlslInputBindDesc{
            .Name = "MaterialSampler",
            .Type = shader::HlslShaderInputType::SAMPLER,
            .BindPoint = 2,
            .BindCount = 1,
            .Space = 2}};
    return result;
}

shader::ShaderBinary MakeShaderBinary() {
    shader::ShaderBinary result;
    result.Asset.AssetId = Guid::NewGuid();
    shader::ShaderPassDesc pass;
    pass.Name = "Material";
    pass.SourcePath = "forward_pipeline/error_pass.hlsl";
    pass.VariantDomain.KeywordGroups = {
        shader::ShaderKeywordGroupDesc{
            .Alternatives = {"", "_ALPHATEST_ON=1", "_ALPHABLEND_ON=1"},
            .Scope = shader::ShaderKeywordScope::Local,
            .Stages = shader::ShaderStage::Vertex},
        shader::ShaderKeywordGroupDesc{
            .Alternatives = {"", "_POINT_SHADOWS=1"},
            .Scope = shader::ShaderKeywordScope::Global,
            .Stages = shader::ShaderStage::Vertex}};
    pass.BakeSet.Variants = {
        shader::ShaderVariantKey{},
        shader::ShaderVariantKey{{"_ALPHATEST_ON=1"}}};
    std::get<shader::ShaderGraphicsPassDesc>(pass.Program).VertexEntry = "VSMain";
    result.Asset.Passes.emplace_back(std::move(pass));

    shader::HlslShaderDesc reflection = MakeReflection();
    const auto payload = shader::SerializeHlslShaderDesc(reflection);
    EXPECT_TRUE(payload.has_value());
    result.Reflections.emplace_back(shader::ShaderReflectionRecord{
        .Target = shader::ShaderTarget::DXIL,
        .Reflection = reflection,
        .Hash = shader::HashShaderBytes(std::as_bytes(std::span{payload->data(), payload->size()}))});
    auto stageInterface = shader::NormalizeHlslInterface(reflection, shader::ShaderStage::Vertex);
    EXPECT_TRUE(stageInterface.Succeeded());
    result.StageInterfaces.emplace_back(std::move(*stageInterface.Interface));
    auto programInterface = shader::MergeGraphicsStageInterfaces(result.StageInterfaces.front());
    EXPECT_TRUE(programInterface.Succeeded());
    result.ProgramInterfaces.emplace_back(std::move(*programInterface.Interface));

    const auto& variants = result.Asset.Passes.front().BakeSet.Variants;
    for (uint32_t i = 0; i < variants.size(); ++i) {
        shader::ShaderStageArtifact artifact{
            .Target = shader::ShaderTarget::DXIL,
            .Category = shader::ShaderBlobCategory::DXIL,
            .PassIndex = 0,
            .Stage = shader::ShaderStage::Vertex,
            .Defines = variants[i].Defines,
            .EntryPoint = "VSMain",
            .Bytecode = {static_cast<byte>(i + 1)},
            .ReflectionIndex = 0,
            .InterfaceIndex = 0};
        artifact.BinaryHash = shader::HashShaderBytes(artifact.Bytecode);
        result.StageArtifacts.emplace_back(std::move(artifact));
        result.ProgramVariants.emplace_back(shader::ShaderProgramVariantArtifact{
            .Target = shader::ShaderTarget::DXIL,
            .PassIndex = 0,
            .Defines = variants[i].Defines,
            .StageArtifactIndices = {i},
            .InterfaceIndex = 0});
    }
    return result;
}

PipelineBindingPolicy MakePolicy() {
    auto provider = make_shared<ShaderBindingSchemaProvider>(
        "TestPipeline",
        vector<ShaderBindingProviderSchemaEntry>{
            ShaderBindingProviderSchemaEntry{
                .AcceptedBindings = {MakeSampler(0, 0, "Expected")}}});
    return PipelineBindingPolicy{{PipelineBindingReservation{.GroupIndex = 0, .Provider = std::move(provider)}}};
}

ShaderResolvedProgram MakeResolvedProgram(
    const ShaderAsset& asset,
    shader::ShaderInterfaceDesc interface,
    vector<string> defines,
    shader::ShaderTarget target = shader::ShaderTarget::DXIL) {
    shader::NormalizeShaderDefines(defines);
    const shader::ShaderPassDesc& pass = asset.GetPasses().front();
    const shader::ShaderHash programIdentity = ComputeShaderProgramIdentity(
        pass,
        0,
        defines,
        pass.SourceIdentity);
    return ShaderResolvedProgram{
        .Target = target,
        .PassIndex = 0,
        .Defines = std::move(defines),
        .Interface = std::move(interface),
        .SourceIdentity = pass.SourceIdentity,
        .ProgramIdentity = programIdentity};
}

}  // namespace

TEST(MaterialAssetTest, SeparatesLocalKeywordsAndProviderGroups) {
    shader::ShaderBinary binary = MakeShaderBinary();
    ASSERT_TRUE(binary.IsValid());
    AssetManager assets;
    StreamingAssetRef<ShaderAsset> shaderRef =
        assets.AddReady(Guid::NewGuid(), make_unique<ShaderAsset>(std::move(binary)));
    ASSERT_TRUE(shaderRef.IsReady());

    MaterialAsset material{shaderRef, MakePolicy()};
    ASSERT_TRUE(material.IsReady());
    ASSERT_EQ(material.GetParameters().GetLayout().Groups().size(), 1u);
    EXPECT_EQ(material.GetParameters().GetLayout().Groups().front().GroupIndex, 2u);
    EXPECT_FALSE(material.GetParameters().GetLayout().FindBinding("PipelineSampler").HasValue());
    EXPECT_TRUE(material.GetParameters().GetLayout().FindBinding("MaterialSampler").HasValue());
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
    EXPECT_TRUE(material.SetSampler("MaterialSampler", render::SamplerDescriptor{}));
    EXPECT_FALSE(material.SetSampler("PipelineSampler", render::SamplerDescriptor{}));
    EXPECT_FALSE(material.SetFloat("Pipeline.Exposure", 1.0f));
}

TEST(MaterialAssetTest, LayoutIsBuiltOncePerShaderAndPolicy) {
    shader::ShaderBinary binary = MakeShaderBinary();
    ASSERT_TRUE(binary.IsValid());
    AssetManager assets;
    StreamingAssetRef<ShaderAsset> shaderRef =
        assets.AddReady(Guid::NewGuid(), make_unique<ShaderAsset>(std::move(binary)));
    ASSERT_TRUE(shaderRef.IsReady());

    MaterialAsset material;
    EXPECT_FALSE(material.IsReady());
    ASSERT_TRUE(material.SetShader(shaderRef, MakePolicy()));
    EXPECT_TRUE(material.IsReady());
    const uint64_t revision = material.GetRevision();
    ASSERT_TRUE(material.RefreshShaderLayout());
    EXPECT_EQ(material.GetRevision(), revision);
}

TEST(MaterialAssetTest, JitInterfacesExtendLayoutWithoutLosingValues) {
    shader::ShaderBinary binary = MakeShaderBinary();
    ASSERT_TRUE(binary.IsValid());
    AssetManager assets;
    StreamingAssetRef<ShaderAsset> shaderRef =
        assets.AddReady(Guid::NewGuid(), make_unique<ShaderAsset>(std::move(binary)));
    MaterialAsset material{shaderRef, MakePolicy()};
    ASSERT_TRUE(material.IsReady());
    render::SamplerDescriptor sampler;
    sampler.LodMax = 9.0f;
    ASSERT_TRUE(material.SetSampler("MaterialSampler", sampler));

    shader::ShaderInterfaceDesc extended = shaderRef->GetBinary().ProgramInterfaces.front();
    extended.BindingGroups.emplace_back(shader::ShaderBindingGroupInterfaceDesc{
        .GroupIndex = 4,
        .Bindings = {MakeSampler(4, 0, "JitSampler")}});
    ShaderResolvedProgram extendedProgram = MakeResolvedProgram(
        *shaderRef.Get(),
        std::move(extended),
        {"_ALPHABLEND_ON=1"});
    ASSERT_TRUE(material.ApplyResolvedPrograms(
        std::span<const ShaderResolvedProgram>{&extendedProgram, 1}));
    ASSERT_EQ(material.GetParameters().GetLayout().Groups().size(), 2u);
    const auto materialValue = std::ranges::find(
        material.GetParameters().Values(),
        ShaderParameterLocation{2, 2},
        &ShaderParameterBindingValue::Location);
    ASSERT_NE(materialValue, material.GetParameters().Values().end());
    ASSERT_EQ(materialValue->Resources.size(), 1u);
    ASSERT_TRUE(std::holds_alternative<render::SamplerDescriptor>(materialValue->Resources.front()));
    EXPECT_EQ(std::get<render::SamplerDescriptor>(materialValue->Resources.front()), sampler);

    shader::ShaderInterfaceDesc second = shaderRef->GetBinary().ProgramInterfaces.front();
    second.BindingGroups.emplace_back(shader::ShaderBindingGroupInterfaceDesc{
        .GroupIndex = 5,
        .Bindings = {MakeSampler(5, 0, "SecondJitSampler")}});
    ShaderResolvedProgram secondProgram = MakeResolvedProgram(
        *shaderRef.Get(),
        std::move(second),
        {"_ALPHABLEND_ON=1", "_POINT_SHADOWS=1"});
    ASSERT_TRUE(material.ApplyResolvedPrograms(
        std::span<const ShaderResolvedProgram>{&secondProgram, 1}));
    ASSERT_EQ(material.GetParameters().GetLayout().Groups().size(), 3u);
    EXPECT_TRUE(material.GetParameters().GetLayout().FindBinding(
                                                        ShaderParameterLocation{4, 0})
                    .HasValue());
    EXPECT_TRUE(material.GetParameters().GetLayout().FindBinding(
                                                        ShaderParameterLocation{5, 0})
                    .HasValue());
    const auto preservedValue = std::ranges::find(
        material.GetParameters().Values(),
        ShaderParameterLocation{2, 2},
        &ShaderParameterBindingValue::Location);
    ASSERT_NE(preservedValue, material.GetParameters().Values().end());
    EXPECT_EQ(std::get<render::SamplerDescriptor>(preservedValue->Resources.front()), sampler);

    shader::ShaderInterfaceDesc conflicting = shaderRef->GetBinary().ProgramInterfaces.front();
    shader::ShaderBindingDesc& conflict = conflicting.BindingGroups[1].Bindings[0];
    conflict.Kind = shader::ShaderBindingKind::SampledTexture;
    conflict.Texture = shader::ShaderTextureInterfaceDesc{
        .Dimension = shader::ShaderTextureDimension::Dim2D,
        .SampleType = shader::ShaderSampleType::Float};
    ShaderResolvedProgram conflictingProgram = MakeResolvedProgram(
        *shaderRef.Get(),
        std::move(conflicting),
        {"_POINT_SHADOWS=1"});
    EXPECT_FALSE(material.ApplyResolvedPrograms(
        std::span<const ShaderResolvedProgram>{&conflictingProgram, 1}));
    EXPECT_EQ(material.GetParameters().GetLayout().Groups().size(), 3u);
}

TEST(MaterialAssetTest, ReadinessIsStructuralAndCompletenessIsProgramSpecific) {
    shader::ShaderBinary binary = MakeShaderBinary();
    ASSERT_TRUE(binary.IsValid());
    AssetManager assets;
    StreamingAssetRef<ShaderAsset> shaderRef =
        assets.AddReady(Guid::NewGuid(), make_unique<ShaderAsset>(std::move(binary)));
    MaterialAsset material{shaderRef, MakePolicy()};
    ASSERT_TRUE(material.IsReady());

    const shader::ShaderInterfaceDesc baked = shaderRef->GetBinary().ProgramInterfaces.front();
    EXPECT_TRUE(material.HasCompleteParametersFor(baked));

    shader::ShaderInterfaceDesc textureVariant = baked;
    textureVariant.BindingGroups.emplace_back(shader::ShaderBindingGroupInterfaceDesc{
        .GroupIndex = 4,
        .Bindings = {MakeTexture(0, "VariantTexture")}});
    ShaderResolvedProgram textureProgram = MakeResolvedProgram(
        *shaderRef.Get(),
        textureVariant,
        {"_ALPHABLEND_ON=1"});
    ASSERT_TRUE(material.ApplyResolvedPrograms(
        std::span<const ShaderResolvedProgram>{&textureProgram, 1}));
    EXPECT_TRUE(material.IsReady());
    EXPECT_FALSE(material.GetParameters().IsComplete());
    EXPECT_TRUE(material.HasCompleteParametersFor(baked));
    EXPECT_FALSE(material.HasCompleteParametersFor(textureVariant));
}

TEST(MaterialAssetTest, RejectsContextualInterfaceThatDisagreesWithBakedProgram) {
    shader::ShaderBinary binary = MakeShaderBinary();
    ASSERT_TRUE(binary.IsValid());
    AssetManager assets;
    StreamingAssetRef<ShaderAsset> shaderRef =
        assets.AddReady(Guid::NewGuid(), make_unique<ShaderAsset>(std::move(binary)));
    MaterialAsset material{shaderRef, MakePolicy()};
    ASSERT_TRUE(material.IsReady());

    shader::ShaderInterfaceDesc mismatched = shaderRef->GetBinary().ProgramInterfaces.front();
    mismatched.BindingGroups.emplace_back(shader::ShaderBindingGroupInterfaceDesc{
        .GroupIndex = 4,
        .Bindings = {MakeSampler(4, 0, "Unexpected")}});
    ShaderResolvedProgram mismatchedProgram = MakeResolvedProgram(
        *shaderRef.Get(),
        std::move(mismatched),
        {"_ALPHATEST_ON=1"},
        shader::ShaderTarget::SPIRV);
    EXPECT_FALSE(material.ApplyResolvedPrograms(
        std::span<const ShaderResolvedProgram>{&mismatchedProgram, 1}));
    ASSERT_EQ(material.GetLayoutDiagnostics().size(), 1u);
    EXPECT_EQ(
        material.GetLayoutDiagnostics().front().Code,
        ShaderBindingDiagnosticCode::InterfaceMismatch);
    ASSERT_TRUE(material.GetLayoutDiagnostics().front().RelatedContext.has_value());
    EXPECT_EQ(material.GetLayoutDiagnostics().front().RelatedContext->PassIndex, 0u);
}

}  // namespace radray
