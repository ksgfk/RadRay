#include <gtest/gtest.h>

#include <radray/runtime/material_layout.h>

namespace radray {
namespace {

constexpr uint32_t kTestMaterialBindingGroup = 7;
constexpr uint32_t kOtherBindingGroup = 11;

shader::HlslShaderDesc MakeHlslMaterialReflection() {
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
        .Name = "Pbr",
        .StartOffset = 16,
        .Size = 16});
    result.BoundResources.emplace_back(shader::HlslInputBindDesc{
        .Name = "gMaterial",
        .Type = shader::HlslShaderInputType::CBUFFER,
        .BindPoint = 0,
        .BindCount = 1,
        .Space = kTestMaterialBindingGroup});
    result.BoundResources.emplace_back(shader::HlslInputBindDesc{
        .Name = "gBaseColorMap",
        .Type = shader::HlslShaderInputType::TEXTURE,
        .BindPoint = 1,
        .BindCount = 1,
        .Dimension = shader::HlslSRVDimension::TEXTURE2D,
        .Space = kTestMaterialBindingGroup});
    result.BoundResources.emplace_back(shader::HlslInputBindDesc{
        .Name = "gSampler",
        .Type = shader::HlslShaderInputType::SAMPLER,
        .BindPoint = 2,
        .BindCount = 1,
        .Space = kTestMaterialBindingGroup});
    result.BoundResources.emplace_back(shader::HlslInputBindDesc{
        .Name = "gView",
        .Type = shader::HlslShaderInputType::CBUFFER,
        .BindPoint = 0,
        .BindCount = 1,
        .Space = kOtherBindingGroup});
    return result;
}

shader::SpirvShaderDesc MakeSpirvMaterialReflection() {
    shader::SpirvShaderDesc result;
    shader::SpirvTypeInfo materialType;
    materialType.Name = "MaterialConstants";
    materialType.BaseType = shader::SpirvBaseType::Struct;
    materialType.Size = 32;
    materialType.Members = {
        shader::SpirvTypeMember{.Name = "BaseColor", .Offset = 0, .Size = 16, .TypeIndex = 1},
        shader::SpirvTypeMember{.Name = "Pbr", .Offset = 16, .Size = 16, .TypeIndex = 1}};
    result.Types.emplace_back(std::move(materialType));
    result.Types.emplace_back(shader::SpirvTypeInfo{
        .Name = "float4",
        .Members = {},
        .BaseType = shader::SpirvBaseType::Float32,
        .VectorSize = 4,
        .Size = 16});
    result.ResourceBindings.emplace_back(shader::SpirvResourceBinding{
        .Name = "gMaterial",
        .Kind = shader::SpirvResourceKind::UniformBuffer,
        .Set = kTestMaterialBindingGroup,
        .Binding = 0,
        .TypeIndex = 0,
        .ImageInfo = {},
        .UniformBufferSize = 32});
    result.ResourceBindings.emplace_back(shader::SpirvResourceBinding{
        .Name = "gBaseColorMap",
        .Kind = shader::SpirvResourceKind::SeparateImage,
        .Set = kTestMaterialBindingGroup,
        .Binding = 1,
        .ImageInfo = {}});
    result.ResourceBindings.emplace_back(shader::SpirvResourceBinding{
        .Name = "gSampler",
        .Kind = shader::SpirvResourceKind::SeparateSampler,
        .Set = kTestMaterialBindingGroup,
        .Binding = 2,
        .ImageInfo = {}});
    return result;
}

TEST(MaterialLayoutTest, MergesBackendsAndIgnoresOtherBindingGroups) {
    vector<shader::CompiledShaderStage> stages(2);
    stages[0].Reflection = MakeHlslMaterialReflection();
    stages[1].Reflection = MakeSpirvMaterialReflection();

    auto layout = BuildMaterialLayout(stages, kTestMaterialBindingGroup);
    ASSERT_TRUE(layout.has_value());
    ASSERT_TRUE(layout->IsValid());
    EXPECT_EQ(layout->Group, kTestMaterialBindingGroup);
    ASSERT_EQ(layout->Bindings.size(), 3u);

    auto constants = layout->FindBinding("gMaterial");
    ASSERT_TRUE(constants.HasValue());
    EXPECT_EQ(constants.Get()->Kind, MaterialBindingKind::ConstantBuffer);
    EXPECT_EQ(constants.Get()->ByteSize, 32u);
    ASSERT_EQ(constants.Get()->Fields.size(), 2u);
    EXPECT_EQ(constants.Get()->Fields[0], (MaterialFieldDesc{"BaseColor", 0, 16}));
    EXPECT_EQ(constants.Get()->Fields[1], (MaterialFieldDesc{"Pbr", 16, 16}));

    EXPECT_TRUE(layout->FindBinding("gBaseColorMap").HasValue());
    EXPECT_TRUE(layout->FindBinding("gSampler").HasValue());
    EXPECT_FALSE(layout->FindBinding("gView").HasValue());
}

TEST(MaterialLayoutTest, RejectsVariantBindingConflicts) {
    vector<shader::CompiledShaderStage> stages(2);
    stages[0].Reflection = MakeHlslMaterialReflection();
    shader::SpirvShaderDesc incompatible = MakeSpirvMaterialReflection();
    incompatible.ResourceBindings[1].Binding = 2;
    stages[1].Reflection = std::move(incompatible);
    EXPECT_FALSE(BuildMaterialLayout(stages, kTestMaterialBindingGroup).has_value());
}

TEST(MaterialLayoutTest, AcceptsBindingGroupsWithoutResources) {
    vector<shader::CompiledShaderStage> stages(1);
    stages[0].Reflection = shader::HlslShaderDesc{};
    auto layout = BuildMaterialLayout(stages, kTestMaterialBindingGroup);
    ASSERT_TRUE(layout.has_value());
    EXPECT_TRUE(layout->Empty());
}

TEST(MaterialLayoutTest, RejectsEmptyStageSet) {
    EXPECT_FALSE(BuildMaterialLayout({}, kTestMaterialBindingGroup).has_value());
}

}  // namespace
}  // namespace radray
