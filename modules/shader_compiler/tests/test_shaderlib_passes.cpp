#include <radray/shader_compiler/client.h>

#include "compiler_test_utils.h"

#include <gtest/gtest.h>

namespace radray::shader_compiler {
namespace {

using namespace test;
using shader::ShaderTarget;

constexpr std::string_view kDepthPass = "modules/shader_compiler/tests/data/depth.hlsl";
constexpr std::string_view kComputePass = "modules/shader_compiler/tests/data/compute.hlsl";

DiscoveryResult DiscoverPass(const Client& client, std::string_view pass) {
    return client.DiscoverSourceContract(
        shader::SourceContractRequest{.SourceName = string{pass}, .RootSource = ReadProjectFile(pass), .Defines = {}, .Targets = shader::ShaderTargetMask::All},
        ShaderlibIncludePaths());
}

TEST(RadRayShaderLibPass, DepthPassIsVertexOnlyGraphics) {
    const Client client;

    const auto discovery = DiscoverPass(client, kDepthPass);

    ASSERT_TRUE(discovery.Succeeded()) << discovery.Diagnostics;
    EXPECT_EQ(discovery.Value->Kind, shader::ShaderKind::Graphics);
    ASSERT_EQ(discovery.Value->EntryPoints.size(), 1u);
    EXPECT_EQ(discovery.Value->EntryPoints[0].Stage, shader::ShaderStage::Vertex);
    ASSERT_EQ(discovery.Value->KeywordGroups.size(), 1u);
    EXPECT_EQ(discovery.Value->KeywordGroups[0].Name, "DEPTH_MODE");
}

TEST(RadRayShaderLibPass, DepthPassCompilesWithoutResourcesOnBothTargets) {
    const Client client;

    const auto result = DiscoverAndCompile(client, {.SourceName = kDepthPass, .Source = ReadProjectFile(kDepthPass), .Assignments = {{"DEPTH_MODE", "regular"}}});

    for (const ShaderTarget target : {ShaderTarget::DXIL, ShaderTarget::SPIRV}) {
        SCOPED_TRACE(static_cast<uint32_t>(target));
        const auto artifact = DecodeLane(result, target);
        ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
        EXPECT_TRUE(artifact.Value->Bindings().empty());
        ASSERT_EQ(artifact.Value->VertexInputs().size(), 1u);
        EXPECT_EQ(NameOf(*artifact.Value, artifact.Value->VertexInputs()[0].Semantic), "POSITION");
    }
}

TEST(RadRayShaderLibPass, ComputePassHasOneComputeEntry) {
    const Client client;

    const auto discovery = DiscoverPass(client, kComputePass);

    ASSERT_TRUE(discovery.Succeeded()) << discovery.Diagnostics;
    EXPECT_EQ(discovery.Value->Kind, shader::ShaderKind::Compute);
    ASSERT_EQ(discovery.Value->EntryPoints.size(), 1u);
    EXPECT_EQ(discovery.Value->EntryPoints[0].Stage, shader::ShaderStage::Compute);
    ASSERT_EQ(discovery.Value->KeywordGroups.size(), 1u);
    EXPECT_EQ(discovery.Value->KeywordGroups[0].Name, "COMPUTE_MODE");
}

TEST(RadRayShaderLibPass, ComputePassPublishesStorageBufferPerTarget) {
    const Client client;

    const auto result = DiscoverAndCompile(client, {.SourceName = kComputePass, .Source = ReadProjectFile(kComputePass), .Assignments = {{"COMPUTE_MODE", "clear"}}});

    for (const ShaderTarget target : {ShaderTarget::DXIL, ShaderTarget::SPIRV}) {
        SCOPED_TRACE(static_cast<uint32_t>(target));
        const auto artifact = DecodeLane(result, target);
        ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
        const auto output = artifact.Value->FindBinding("Output");
        ASSERT_TRUE(output.has_value());
        EXPECT_EQ(output->Record.Type, static_cast<uint32_t>(shader::ShaderBindingKind::RWStructuredBuffer));
        EXPECT_EQ(output->Record.Group, target == ShaderTarget::DXIL ? 0u : 2u);
        EXPECT_EQ(output->Record.Binding, target == ShaderTarget::DXIL ? 0u : 6u);
        EXPECT_EQ(output->Record.StageMask, kComputeBit);
    }
}

}  // namespace
}  // namespace radray::shader_compiler
