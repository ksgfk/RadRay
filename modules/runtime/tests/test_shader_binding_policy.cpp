#include <gtest/gtest.h>

#include <radray/runtime/shader_parameters.h>

namespace radray {
namespace {

shader::ShaderBindingDesc MakeSampler(
    uint32_t binding,
    std::string_view name = "Sampler",
    uint32_t count = 1) {
    return shader::ShaderBindingDesc{
        .Name = string{name},
        .BindingIndex = binding,
        .Kind = shader::ShaderBindingKind::Sampler,
        .Access = shader::ShaderResourceAccess::ReadOnly,
        .Count = count,
        .Stages = shader::ShaderStage::Pixel};
}

shader::ShaderBindingDesc MakeConstants(
    uint32_t byteSize,
    vector<shader::ShaderInterfaceFieldDesc> fields) {
    return shader::ShaderBindingDesc{
        .Name = "Constants",
        .BindingIndex = 0,
        .Kind = shader::ShaderBindingKind::ConstantBuffer,
        .Access = shader::ShaderResourceAccess::ReadOnly,
        .Count = 1,
        .Stages = shader::ShaderStage::Pixel,
        .Buffer = shader::ShaderBufferInterfaceDesc{
            .ByteSize = byteSize,
            .Fields = std::move(fields)}};
}

shader::ShaderInterfaceFieldDesc MakeFloat4(std::string_view name, uint32_t offset) {
    return shader::ShaderInterfaceFieldDesc{
        .Name = string{name},
        .Offset = offset,
        .Size = 16,
        .Type = shader::ShaderValueTypeDesc{
            .Scalar = shader::ShaderScalarType::Float32,
            .Rows = 1,
            .Columns = 4,
            .ArrayCount = 1,
            .ByteSize = 16}};
}

shader::ShaderInterfaceDesc MakeInterface(
    vector<shader::ShaderBindingGroupInterfaceDesc> groups) {
    return shader::ShaderInterfaceDesc{
        .Kind = shader::ShaderProgramKind::Graphics,
        .BindingGroups = std::move(groups)};
}

shared_ptr<const IShaderBindingProvider> MakeProvider() {
    return make_shared<ShaderBindingSchemaProvider>(
        "TestPipeline",
        vector<ShaderBindingProviderSchemaEntry>{
            ShaderBindingProviderSchemaEntry{
                .AcceptedBindings = {MakeSampler(0)},
                .Required = false}});
}

}  // namespace

TEST(ShaderBindingPolicyTest, MissingReservedGroupsAndCustomShadersAreValid) {
    PipelineBindingPolicy policy{{PipelineBindingReservation{.GroupIndex = 0, .Provider = MakeProvider()}}};
    shader::ShaderInterfaceDesc custom = MakeInterface({shader::ShaderBindingGroupInterfaceDesc{.GroupIndex = 4, .Bindings = {MakeSampler(2, "UserSampler")}}});
    auto result = ResolveShaderBindings(custom, policy);
    ASSERT_TRUE(result.Succeeded());
    ASSERT_EQ(result.Plan->ProviderGroups.size(), 0u);
    ASSERT_EQ(result.Plan->UserGroups.size(), 1u);
    EXPECT_EQ(result.Plan->UserGroups.front().GroupIndex, 4u);

    auto noParameters = ResolveShaderBindings(MakeInterface({}), policy);
    ASSERT_TRUE(noParameters.Succeeded());
    EXPECT_TRUE(noParameters.Plan->ProviderGroups.empty());
    EXPECT_TRUE(noParameters.Plan->UserGroups.empty());
}

TEST(ShaderBindingPolicyTest, ProviderOwnsCompatibleWholeGroup) {
    PipelineBindingPolicy policy{{PipelineBindingReservation{.GroupIndex = 1, .Provider = MakeProvider()}}};
    shader::ShaderInterfaceDesc interface = MakeInterface({shader::ShaderBindingGroupInterfaceDesc{.GroupIndex = 1, .Bindings = {MakeSampler(0, "AnyName")}},
                                                           shader::ShaderBindingGroupInterfaceDesc{.GroupIndex = 3, .Bindings = {MakeSampler(5, "MaterialSampler")}}});
    auto result = ResolveShaderBindings(interface, policy);
    ASSERT_TRUE(result.Succeeded());
    ASSERT_EQ(result.Plan->ProviderGroups.size(), 1u);
    EXPECT_EQ(result.Plan->ProviderGroups.front().GroupIndex, 1u);
    ASSERT_EQ(result.Plan->UserGroups.size(), 1u);
    EXPECT_EQ(result.Plan->UserGroups.front().GroupIndex, 3u);
}

TEST(ShaderBindingPolicyTest, UnknownReservedBindingIsIncompatible) {
    PipelineBindingPolicy policy{{PipelineBindingReservation{.GroupIndex = 1, .Provider = MakeProvider()}}};
    shader::ShaderInterfaceDesc interface = MakeInterface({shader::ShaderBindingGroupInterfaceDesc{.GroupIndex = 1, .Bindings = {MakeSampler(7, "Unknown")}}});
    shader::ShaderDiagnosticContext context;
    context.PassIndex = 2;
    auto result = ResolveShaderBindings(interface, policy, context);
    ASSERT_FALSE(result.Succeeded());
    ASSERT_EQ(result.Diagnostics.size(), 1u);
    EXPECT_EQ(result.Diagnostics.front().Code, ShaderBindingDiagnosticCode::ProviderRejectedGroup);
    EXPECT_EQ(result.Diagnostics.front().Context.PassIndex, 2u);
    EXPECT_EQ(result.Diagnostics.front().Context.Group, 1u);
    EXPECT_EQ(result.Diagnostics.front().Context.Binding, 7u);
}

TEST(ShaderBindingPolicyTest, EmptyPolicyTreatsEveryGroupAsUserOwned) {
    shader::ShaderInterfaceDesc interface = MakeInterface({shader::ShaderBindingGroupInterfaceDesc{.GroupIndex = 0, .Bindings = {MakeSampler(0)}}});
    auto result = ResolveShaderBindings(interface, PipelineBindingPolicy{});
    ASSERT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Plan->ProviderGroups.empty());
    ASSERT_EQ(result.Plan->UserGroups.size(), 1u);
    EXPECT_EQ(result.Plan->UserGroups.front().GroupIndex, 0u);
}

TEST(ShaderBindingPolicyTest, ProviderSchemaSupportsExplicitPhysicalAlternatives) {
    auto provider = make_shared<ShaderBindingSchemaProvider>(
        "AlternativeProvider",
        vector<ShaderBindingProviderSchemaEntry>{ShaderBindingProviderSchemaEntry{
            .AcceptedBindings = {
                MakeSampler(0, "Single", 1),
                MakeSampler(0, "Array", 2)}}});
    PipelineBindingPolicy policy{{PipelineBindingReservation{
        .GroupIndex = 1,
        .Provider = std::move(provider)}}};

    auto compatible = ResolveShaderBindings(
        MakeInterface({shader::ShaderBindingGroupInterfaceDesc{
            .GroupIndex = 1,
            .Bindings = {MakeSampler(0, "UserName", 2)}}}),
        policy);
    ASSERT_TRUE(compatible.Succeeded());

    auto incompatible = ResolveShaderBindings(
        MakeInterface({shader::ShaderBindingGroupInterfaceDesc{
            .GroupIndex = 1,
            .Bindings = {MakeSampler(0, "UserName", 3)}}}),
        policy);
    ASSERT_FALSE(incompatible.Succeeded());
    ASSERT_EQ(incompatible.Diagnostics.size(), 1u);
    EXPECT_EQ(incompatible.Diagnostics.front().Context.Group, 1u);
    EXPECT_EQ(incompatible.Diagnostics.front().Context.Binding, 0u);
}

TEST(ShaderBindingPolicyTest, ProviderAcceptsConstantBufferFieldProjection) {
    auto provider = make_shared<ShaderBindingSchemaProvider>(
        "ConstantProvider",
        vector<ShaderBindingProviderSchemaEntry>{ShaderBindingProviderSchemaEntry{
            .AcceptedBindings = {MakeConstants(
                32,
                {MakeFloat4("View", 0), MakeFloat4("Lighting", 16)})}}});
    PipelineBindingPolicy policy{{PipelineBindingReservation{
        .GroupIndex = 1,
        .Provider = std::move(provider)}}};

    auto projected = ResolveShaderBindings(
        MakeInterface({shader::ShaderBindingGroupInterfaceDesc{
            .GroupIndex = 1,
            .Bindings = {MakeConstants(16, {MakeFloat4("Renamed", 0)})}}}),
        policy);
    ASSERT_TRUE(projected.Succeeded());

    auto incompatible = ResolveShaderBindings(
        MakeInterface({shader::ShaderBindingGroupInterfaceDesc{
            .GroupIndex = 1,
            .Bindings = {MakeConstants(32, {MakeFloat4("Wrong", 8)})}}}),
        policy);
    ASSERT_FALSE(incompatible.Succeeded());
    EXPECT_EQ(incompatible.Diagnostics.front().Context.Binding, 0u);
}

}  // namespace radray
