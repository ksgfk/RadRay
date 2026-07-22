#include <array>

#include <gtest/gtest.h>

#include <radray/runtime/material_asset.h>
#include <radray/runtime/shader_artifact_resolver.h>
#include <radray/runtime/shader_asset.h>
#include <radray/runtime/shader_binding_policy.h>

namespace radray {
namespace {

render::ShaderBindingDesc MakeSampler(
    uint32_t group,
    uint32_t binding,
    std::string_view name) {
    (void)group;
    return render::ShaderBindingDesc{
        .Name = string{name},
        .BindingIndex = binding,
        .Kind = render::ShaderBindingKind::Sampler,
        .Access = render::ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Stages = render::ShaderStage::Vertex};
}

render::ShaderBindingDesc MakeTexture(
    uint32_t binding,
    std::string_view name) {
    return render::ShaderBindingDesc{
        .Name = string{name},
        .BindingIndex = binding,
        .Kind = render::ShaderBindingKind::SampledTexture,
        .Access = render::ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Stages = render::ShaderStage::Pixel,
        .Texture = render::ShaderTextureInterfaceDesc{
            .Dimension = render::ShaderTextureDimension::Dim2D,
            .SampleType = render::ShaderSampleType::Float}};
}

render::HlslShaderDesc MakeReflection() {
    render::HlslShaderDesc result;
    result.BoundResources = {
        render::HlslInputBindDesc{
            .Name = "PipelineSampler",
            .Type = render::HlslShaderInputType::SAMPLER,
            .BindPoint = 0,
            .BindCount = 1,
            .Space = 0},
        render::HlslInputBindDesc{
            .Name = "MaterialSampler",
            .Type = render::HlslShaderInputType::SAMPLER,
            .BindPoint = 2,
            .BindCount = 1,
            .Space = 2}};
    return result;
}

render::ShaderBinary MakeShaderBinary() {
    render::ShaderBinary result;
    result.Asset.AssetId = Guid::NewGuid();
    render::ShaderPassDesc pass;
    pass.Name = "Material";
    pass.SourcePath = "forward_pipeline/error_pass.hlsl";
    pass.VariantDomain.KeywordGroups = {
        render::ShaderKeywordGroupDesc{
            .Alternatives = {"", "_ALPHATEST_ON=1", "_ALPHABLEND_ON=1"},
            .Scope = render::ShaderKeywordScope::Local,
            .Stages = render::ShaderStage::Vertex},
        render::ShaderKeywordGroupDesc{
            .Alternatives = {"", "_POINT_SHADOWS=1"},
            .Scope = render::ShaderKeywordScope::Global,
            .Stages = render::ShaderStage::Vertex}};
    pass.BakeSet.Variants = {
        render::ShaderVariantKey{},
        render::ShaderVariantKey{{"_ALPHATEST_ON=1"}}};
    std::get<render::ShaderGraphicsPassDesc>(pass.Program).VertexEntry = "VSMain";
    result.Asset.Passes.emplace_back(std::move(pass));

    render::HlslShaderDesc reflection = MakeReflection();
    const auto payload = render::SerializeHlslShaderDesc(reflection);
    EXPECT_TRUE(payload.has_value());
    result.Reflections.emplace_back(render::ShaderReflectionRecord{
        .Target = render::ShaderTarget::DXIL,
        .Reflection = reflection,
        .Hash = render::HashShaderBytes(std::as_bytes(std::span{payload->data(), payload->size()}))});
    auto stageInterface = render::NormalizeHlslInterface(reflection, render::ShaderStage::Vertex);
    EXPECT_TRUE(stageInterface.Succeeded());
    result.StageInterfaces.emplace_back(std::move(*stageInterface.Interface));
    auto programInterface = render::MergeGraphicsStageInterfaces(result.StageInterfaces.front());
    EXPECT_TRUE(programInterface.Succeeded());
    result.ProgramInterfaces.emplace_back(std::move(*programInterface.Interface));

    const auto& variants = result.Asset.Passes.front().BakeSet.Variants;
    for (uint32_t i = 0; i < variants.size(); ++i) {
        render::ShaderStageArtifact artifact{
            .Target = render::ShaderTarget::DXIL,
            .Category = render::ShaderBlobCategory::DXIL,
            .PassIndex = 0,
            .Stage = render::ShaderStage::Vertex,
            .Defines = variants[i].Defines,
            .EntryPoint = "VSMain",
            .Bytecode = {static_cast<byte>(i + 1)},
            .ReflectionIndex = 0,
            .InterfaceIndex = 0};
        artifact.BinaryHash = render::HashShaderBytes(artifact.Bytecode);
        result.StageArtifacts.emplace_back(std::move(artifact));
        result.ProgramVariants.emplace_back(render::ShaderProgramVariantArtifact{
            .Target = render::ShaderTarget::DXIL,
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
    render::ShaderInterfaceDesc interface,
    vector<string> defines,
    render::ShaderTarget target = render::ShaderTarget::DXIL) {
    render::NormalizeShaderDefines(defines);
    const render::ShaderPassDesc& pass = asset.GetPasses().front();
    const render::ShaderHash programIdentity = ComputeShaderProgramIdentity(
        pass,
        0,
        defines,
        pass.SourceIdentity);
    return ShaderResolvedProgram{
        .Target = target,
        .PassIndex = 0,
        .Defines = std::move(defines),
        .Stages = {},
        .Interface = std::move(interface),
        .SourceIdentity = pass.SourceIdentity,
        .ProgramIdentity = programIdentity};
}

}  // namespace

TEST(MaterialAssetTest, SeparatesLocalKeywordsAndProviderGroups) {
    render::ShaderBinary binary = MakeShaderBinary();
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
    render::ShaderBinary binary = MakeShaderBinary();
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
    render::ShaderBinary binary = MakeShaderBinary();
    ASSERT_TRUE(binary.IsValid());
    AssetManager assets;
    StreamingAssetRef<ShaderAsset> shaderRef =
        assets.AddReady(Guid::NewGuid(), make_unique<ShaderAsset>(std::move(binary)));
    MaterialAsset material{shaderRef, MakePolicy()};
    ASSERT_TRUE(material.IsReady());
    render::SamplerDescriptor sampler;
    sampler.LodMax = 9.0f;
    ASSERT_TRUE(material.SetSampler("MaterialSampler", sampler));

    render::ShaderInterfaceDesc extended = shaderRef->GetBinary().ProgramInterfaces.front();
    extended.BindingGroups.emplace_back(render::ShaderBindingGroupInterfaceDesc{
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

    render::ShaderInterfaceDesc second = shaderRef->GetBinary().ProgramInterfaces.front();
    second.BindingGroups.emplace_back(render::ShaderBindingGroupInterfaceDesc{
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

    render::ShaderInterfaceDesc conflicting = shaderRef->GetBinary().ProgramInterfaces.front();
    render::ShaderBindingDesc& conflict = conflicting.BindingGroups[1].Bindings[0];
    conflict.Kind = render::ShaderBindingKind::SampledTexture;
    conflict.Texture = render::ShaderTextureInterfaceDesc{
        .Dimension = render::ShaderTextureDimension::Dim2D,
        .SampleType = render::ShaderSampleType::Float};
    ShaderResolvedProgram conflictingProgram = MakeResolvedProgram(
        *shaderRef.Get(),
        std::move(conflicting),
        {"_POINT_SHADOWS=1"});
    EXPECT_FALSE(material.ApplyResolvedPrograms(
        std::span<const ShaderResolvedProgram>{&conflictingProgram, 1}));
    EXPECT_EQ(material.GetParameters().GetLayout().Groups().size(), 3u);
}

TEST(MaterialAssetTest, ReadinessIsStructuralAndCompletenessIsProgramSpecific) {
    render::ShaderBinary binary = MakeShaderBinary();
    ASSERT_TRUE(binary.IsValid());
    AssetManager assets;
    StreamingAssetRef<ShaderAsset> shaderRef =
        assets.AddReady(Guid::NewGuid(), make_unique<ShaderAsset>(std::move(binary)));
    MaterialAsset material{shaderRef, MakePolicy()};
    ASSERT_TRUE(material.IsReady());

    const render::ShaderInterfaceDesc baked = shaderRef->GetBinary().ProgramInterfaces.front();
    EXPECT_TRUE(material.HasCompleteParametersFor(baked));

    render::ShaderInterfaceDesc textureVariant = baked;
    textureVariant.BindingGroups.emplace_back(render::ShaderBindingGroupInterfaceDesc{
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
    render::ShaderBinary binary = MakeShaderBinary();
    ASSERT_TRUE(binary.IsValid());
    AssetManager assets;
    StreamingAssetRef<ShaderAsset> shaderRef =
        assets.AddReady(Guid::NewGuid(), make_unique<ShaderAsset>(std::move(binary)));
    MaterialAsset material{shaderRef, MakePolicy()};
    ASSERT_TRUE(material.IsReady());

    render::ShaderInterfaceDesc mismatched = shaderRef->GetBinary().ProgramInterfaces.front();
    mismatched.BindingGroups.emplace_back(render::ShaderBindingGroupInterfaceDesc{
        .GroupIndex = 4,
        .Bindings = {MakeSampler(4, 0, "Unexpected")}});
    ShaderResolvedProgram mismatchedProgram = MakeResolvedProgram(
        *shaderRef.Get(),
        std::move(mismatched),
        {"_ALPHATEST_ON=1"},
        render::ShaderTarget::SPIRV);
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
