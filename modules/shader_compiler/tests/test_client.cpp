#include <radray/shader_compiler/client.h>

#include "compiler_test_utils.h"

#include <gtest/gtest.h>

namespace radray::shader_compiler {
namespace {

using namespace test;

constexpr std::string_view kMissingLibrary = "radray_missing_shader_compiler_probe";

constexpr std::string_view kGraphicsSource = R"hlsl(
#pragma radray_keyword_group QUALITY "low" "high"

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
#if defined(TINT_RED)
    return float4(1.0, 0.0, 0.0, 1.0);
#else
    return float4(0.0, 0.0, 1.0, 1.0);
#endif
}
)hlsl";

constexpr std::string_view kVertexOnlySource = R"hlsl(
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}
)hlsl";

VariantInput Graphics(shader::ShaderTargetMask targets = shader::ShaderTargetMask::All) {
    return {.SourceName = "client/graphics.hlsl", .Source = ToBytes(kGraphicsSource), .Targets = targets, .Assignments = {{"QUALITY", "low"}}};
}

VariantInput VertexOnly() {
    return {.SourceName = "client/vertex_only.hlsl", .Source = ToBytes(kVertexOnlySource), .Targets = shader::ShaderTargetMask::DXIL};
}

DiscoveryResult Discover(const Client& client, std::string_view source, std::span<const std::filesystem::path> includePaths = {}) {
    return client.DiscoverSourceContract(
        shader::SourceContractRequest{.SourceName = "client/discover.hlsl", .RootSource = ToBytes(source), .Defines = {}, .Targets = shader::ShaderTargetMask::All},
        includePaths);
}

// --- Loading ---

TEST(RadRayShaderCompilerClient, LoadsCanonicalCompilerLibrary) {
    const Client client;

    EXPECT_TRUE(client.IsAvailable());
    EXPECT_TRUE(client.GetToolchainIdentity().has_value());
}

TEST(RadRayShaderCompilerClient, MissingLibraryIsUnavailable) {
    const Client client{kMissingLibrary};

    EXPECT_FALSE(client.IsAvailable());
    EXPECT_FALSE(client.GetToolchainIdentity().has_value());
}

TEST(RadRayShaderCompilerClient, UnavailableClientFailsWithSourceContext) {
    const Client client{kMissingLibrary};
    const VariantInput input = Graphics();

    const auto discovered = client.DiscoverSourceContract("client/graphics.hlsl", input.Source, shader::ShaderTarget::DXIL, {});
    const auto compiled = client.CompileVariant(
        shader::CompileVariantRequest{.SourceName = "client/graphics.hlsl", .RootSource = input.Source, .Defines = {}, .Assignments = {}, .Targets = shader::ShaderTargetMask::All},
        {});

    EXPECT_FALSE(discovered.Succeeded());
    EXPECT_NE(discovered.Diagnostics.find("client/graphics.hlsl"), string::npos);
    EXPECT_FALSE(compiled.Succeeded());
    EXPECT_NE(compiled.Diagnostics.find("client/graphics.hlsl"), string::npos);
}

// --- Discovery ---

TEST(RadRayShaderCompilerClient, DiscoveryReportsGraphicsTopologyAndKeywordDomain) {
    const Client client;

    const auto discovery = Discover(client, kGraphicsSource);

    ASSERT_TRUE(discovery.Succeeded()) << discovery.Diagnostics;
    const shader::ShaderContract& contract = *discovery.Value;
    EXPECT_EQ(contract.Kind, shader::ShaderKind::Graphics);
    ASSERT_EQ(contract.EntryPoints.size(), 2u);
    EXPECT_EQ(contract.EntryPoints[0].Name, "VSMain");
    EXPECT_EQ(contract.EntryPoints[0].Stage, shader::ShaderStage::Vertex);
    EXPECT_EQ(contract.EntryPoints[1].Name, "PSMain");
    EXPECT_EQ(contract.EntryPoints[1].Stage, shader::ShaderStage::Pixel);
    ASSERT_EQ(contract.KeywordGroups.size(), 1u);
    EXPECT_EQ(contract.KeywordGroups[0].Name, "QUALITY");
    EXPECT_EQ(contract.KeywordGroups[0].Values, (vector<string>{"high", "low"}));
}

TEST(RadRayShaderCompilerClient, DiscoveryReportsComputeTopology) {
    const Client client;

    const auto discovery = Discover(client, R"hlsl(
[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {}
)hlsl");

    ASSERT_TRUE(discovery.Succeeded()) << discovery.Diagnostics;
    EXPECT_EQ(discovery.Value->Kind, shader::ShaderKind::Compute);
    ASSERT_EQ(discovery.Value->EntryPoints.size(), 1u);
    EXPECT_EQ(discovery.Value->EntryPoints[0].Stage, shader::ShaderStage::Compute);
}

TEST(RadRayShaderCompilerClient, DiscoveryRejectsMixedGraphicsAndCompute) {
    const Client client;

    const auto discovery = Discover(client, R"hlsl(
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {}
)hlsl");

    EXPECT_FALSE(discovery.Succeeded());
    EXPECT_FALSE(discovery.Diagnostics.empty());
}

TEST(RadRayShaderCompilerClient, DiscoveryRejectsTopologyThatDiffersBetweenTargets) {
    const Client client;

    const auto discovery = Discover(client, R"hlsl(
#if defined(__spirv__)
#define PIXEL_ENTRY PSMainSpirv
#else
#define PIXEL_ENTRY PSMainDxil
#endif

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }

[shader("pixel")]
float4 PIXEL_ENTRY() : SV_Target0 { return float4(1.0, 1.0, 1.0, 1.0); }
)hlsl");

    EXPECT_FALSE(discovery.Succeeded());
    EXPECT_FALSE(discovery.Diagnostics.empty());
}

TEST(RadRayShaderCompilerClient, DiscoveryUsesRequestedShaderModel) {
    const Client client;
    shader::SourceContractRequest request{
        .SourceName = "client/descriptor_heap.hlsl",
        .RootSource = ToBytes(R"hlsl(
[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    RWStructuredBuffer<uint> destination = ResourceDescriptorHeap[0];
    destination[id.x] = id.x;
}
)hlsl"),
        .Defines = {},
        .Targets = shader::ShaderTargetMask::DXIL,
        .Policy = {.ShaderModel = 66}};

    const auto supported = client.DiscoverSourceContract(request, {});
    request.Policy.ShaderModel = 60;
    const auto tooOld = client.DiscoverSourceContract(request, {});

    EXPECT_TRUE(supported.Succeeded()) << supported.Diagnostics;
    EXPECT_FALSE(tooOld.Succeeded());
}

TEST(RadRayShaderCompilerClient, DiscoveryAcceptsEverySupportedShaderModel) {
    const Client client;
    for (uint32_t shaderModel = 60; shaderModel <= 69; ++shaderModel) {
        const auto discovery = client.DiscoverSourceContract(
            shader::SourceContractRequest{
                .SourceName = "client/vertex_only.hlsl",
                .RootSource = ToBytes(kVertexOnlySource),
                .Defines = {},
                .Targets = shader::ShaderTargetMask::All,
                .Policy = {.ShaderModel = shaderModel}},
            {});

        EXPECT_TRUE(discovery.Succeeded()) << "SM " << shaderModel << ": " << discovery.Diagnostics;
    }
}

// --- Variant compilation ---

TEST(RadRayShaderCompilerClient, CompileReturnsLanePerRequestedTarget) {
    const Client client;

    const auto result = DiscoverAndCompile(client, Graphics(shader::ShaderTargetMask::All));

    ASSERT_TRUE(result.Succeeded()) << result.Diagnostics;
    ASSERT_EQ(result.Value->size(), 2u);
    for (const shader::CompileTargetLane& lane : *result.Value) {
        SCOPED_TRACE(static_cast<uint32_t>(lane.Target));
        EXPECT_FALSE(lane.Bytecode.empty());
        EXPECT_FALSE(lane.Metadata.empty());
        ASSERT_EQ(lane.Stages.size(), 2u);
        EXPECT_EQ(lane.Stages[0].Stage, shader::ShaderStage::Vertex);
        EXPECT_EQ(lane.Stages[0].EntryPoint, "VSMain");
        EXPECT_FALSE(lane.Stages[0].Bytecode.empty());
        EXPECT_EQ(lane.Stages[1].Stage, shader::ShaderStage::Pixel);
        EXPECT_EQ(lane.Stages[1].EntryPoint, "PSMain");
        EXPECT_FALSE(lane.Stages[1].Bytecode.empty());
    }
    EXPECT_NE((*result.Value)[0].Target, (*result.Value)[1].Target);
}

TEST(RadRayShaderCompilerClient, CompileOmitsUnrequestedTarget) {
    const Client client;

    const auto result = DiscoverAndCompile(client, Graphics(shader::ShaderTargetMask::SPIRV));

    ASSERT_TRUE(result.Succeeded()) << result.Diagnostics;
    ASSERT_EQ(result.Value->size(), 1u);
    EXPECT_EQ(result.Value->front().Target, shader::ShaderTarget::SPIRV);
}

TEST(RadRayShaderCompilerClient, CompileIsDeterministic) {
    const Client client;

    const auto first = DiscoverAndCompile(client, Graphics());
    const auto second = DiscoverAndCompile(client, Graphics());

    ASSERT_TRUE(first.Succeeded()) << first.Diagnostics;
    ASSERT_TRUE(second.Succeeded()) << second.Diagnostics;
    ASSERT_EQ(first.Value->size(), second.Value->size());
    for (size_t index = 0; index < first.Value->size(); ++index) {
        EXPECT_EQ((*first.Value)[index].Bytecode, (*second.Value)[index].Bytecode);
        EXPECT_EQ((*first.Value)[index].Metadata, (*second.Value)[index].Metadata);
    }
}

TEST(RadRayShaderCompilerClient, DefinesReachThePreprocessor) {
    const Client client;
    VariantInput tinted = Graphics(shader::ShaderTargetMask::DXIL);
    tinted.Defines = {{"TINT_RED", "1"}};

    const auto plain = DiscoverAndCompile(client, Graphics(shader::ShaderTargetMask::DXIL));
    const auto red = DiscoverAndCompile(client, tinted);

    ASSERT_TRUE(plain.Succeeded()) << plain.Diagnostics;
    ASSERT_TRUE(red.Succeeded()) << red.Diagnostics;
    EXPECT_NE(plain.Value->front().Bytecode, red.Value->front().Bytecode);
}

TEST(RadRayShaderCompilerClient, CompileRejectsContractDrift) {
    const Client client;
    const VariantInput input = Graphics();
    shader::CompileVariantRequest request{
        .SourceName = string{input.SourceName},
        .RootSource = input.Source,
        .Defines = {},
        .Assignments = input.Assignments,
        .Targets = input.Targets};
    request.ExpectedContract.Bytes.fill(0xff);

    const auto result = client.CompileVariant(request, {});

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

TEST(RadRayShaderCompilerClient, CompileRequiresAssignmentForEveryKeywordGroup) {
    const Client client;
    VariantInput input = Graphics();
    input.Assignments.clear();

    const auto result = DiscoverAndCompile(client, input);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

TEST(RadRayShaderCompilerClient, CompileRejectsValueOutsideKeywordDomain) {
    const Client client;
    VariantInput input = Graphics();
    input.Assignments = {{"QUALITY", "ultra"}};

    const auto result = DiscoverAndCompile(client, input);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

TEST(RadRayShaderCompilerClient, CompileRejectsAssignmentThatChangesEntryTopology) {
    const Client client;
    const VariantInput input{
        .SourceName = "client/keyword_entry.hlsl",
        .Source = ToBytes(R"hlsl(
#pragma radray_keyword_group PIXEL_ENTRY "PSLow" "PSHigh"

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1.0); }

[shader("pixel")]
float4 PIXEL_ENTRY() : SV_Target0 { return float4(1.0, 1.0, 1.0, 1.0); }
)hlsl"),
        .Targets = shader::ShaderTargetMask::DXIL,
        .Assignments = {{"PIXEL_ENTRY", "PSLow"}}};

    const auto result = DiscoverAndCompile(client, input);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

TEST(RadRayShaderCompilerClient, CompileRejectsNonLogicalSourceName) {
    const Client client;

    const auto result = client.CompileVariant(
        shader::CompileVariantRequest{.SourceName = "../graphics.hlsl", .RootSource = ToBytes(kVertexOnlySource), .Defines = {}, .Assignments = {}, .Targets = shader::ShaderTargetMask::DXIL},
        {});

    EXPECT_FALSE(result.Succeeded());
    EXPECT_NE(result.Diagnostics.find("SourceName"), string::npos);
}

TEST(RadRayShaderCompilerClient, InvalidSourceFailsWithoutLanes) {
    const Client client;
    const VariantInput input{
        .SourceName = "client/invalid.hlsl",
        .Source = ToBytes(R"hlsl(
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return;
}
)hlsl")};

    const auto result = DiscoverAndCompile(client, input);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

TEST(RadRayShaderCompilerClient, LaneFailureFailsWholeBatch) {
    const Client client;
    // Valid for DXIL, but the SPIR-V lane rejects the missing VK_BINDING.
    const VariantInput input{
        .SourceName = "client/dxil_only_binding.hlsl",
        .Source = ToBytes(R"hlsl(
Texture2D<float4> Albedo : register(t0);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return Albedo.Load(int3(0, 0, 0)) + float4(position, 1.0);
}
)hlsl")};

    const auto result = DiscoverAndCompile(client, input);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

TEST(RadRayShaderCompilerClient, AcceptsSupportedNonDefaultPolicy) {
    const Client client;
    VariantInput input = VertexOnly();
    input.Targets = shader::ShaderTargetMask::All;
    input.Policy = {
        .ShaderModel = 65,
        .Optimize = 0,
        .DebugInfo = 1,
        .AllResourcesBound = 1,
        .Warnings = shader::WarningPolicy::WarningsAsErrors,
        .HlslVersion = 2018};

    const auto result = DiscoverAndCompile(client, input);

    ASSERT_TRUE(result.Succeeded()) << result.Diagnostics;
    EXPECT_EQ(result.Value->size(), 2u);
}

TEST(RadRayShaderCompilerClient, AcceptsEverySupportedHlslVersion) {
    const Client client;
    for (const uint32_t version : {2016u, 2017u, 2018u, 2021u}) {
        VariantInput input = VertexOnly();
        input.Policy.HlslVersion = version;

        const auto result = DiscoverAndCompile(client, input);

        EXPECT_TRUE(result.Succeeded()) << "HLSL " << version << ": " << result.Diagnostics;
    }
}

TEST(RadRayShaderCompilerClient, RejectsUnsupportedPolicyValues) {
    const Client client;
    const auto discovery = client.DiscoverSourceContract("client/vertex_only.hlsl", ToBytes(kVertexOnlySource), shader::ShaderTarget::DXIL, {});
    ASSERT_TRUE(discovery.Succeeded()) << discovery.Diagnostics;
    const shader::CompilePolicy invalid[] = {
        {.ShaderModel = 59},
        {.Optimize = 2},
        {.DebugInfo = 2},
        {.AllResourcesBound = 2},
        {.Warnings = static_cast<shader::WarningPolicy>(2)},
        {.SpirvTargetEnv = static_cast<shader::SpirvTargetEnvironment>(1)},
        {.HlslVersion = 2020},
        {.Reserved = 1},
    };

    for (size_t index = 0; index < std::size(invalid); ++index) {
        const auto result = client.CompileVariant(
            shader::CompileVariantRequest{
                .SourceName = "client/vertex_only.hlsl",
                .RootSource = ToBytes(kVertexOnlySource),
                .Defines = {},
                .Assignments = {},
                .Targets = shader::ShaderTargetMask::DXIL,
                .Policy = invalid[index],
                .ExpectedContract = discovery.Value->Hash},
            {});

        EXPECT_FALSE(result.Succeeded()) << "policy case " << index;
        EXPECT_FALSE(result.Diagnostics.empty()) << "policy case " << index;
    }
}

// --- Include paths ---

// includes/first and includes/second both provide shadow.hlsli with a different register.
constexpr std::string_view kShadowIncludeSource = R"hlsl(
#include <shadow.hlsli>

Texture2D<float4> ShadowTexture : register(SHADOW_REGISTER);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return ShadowTexture.Load(int3(0, 0, 0)) + float4(position, 1.0);
}
)hlsl";

TEST(RadRayShaderCompilerClient, EarlierIncludePathShadowsLater) {
    const Client client;
    const vector<std::filesystem::path> firstThenSecond{
        ProjectPath("modules/shader_compiler/tests/data/includes/first"),
        ProjectPath("modules/shader_compiler/tests/data/includes/second")};
    const vector<std::filesystem::path> secondThenFirst{firstThenSecond[1], firstThenSecond[0]};
    const VariantInput input{
        .SourceName = "client/shadow.hlsl",
        .Source = ToBytes(kShadowIncludeSource),
        .Targets = shader::ShaderTargetMask::DXIL};

    const auto first = DecodeLane(DiscoverAndCompile(client, input, firstThenSecond), shader::ShaderTarget::DXIL);
    const auto second = DecodeLane(DiscoverAndCompile(client, input, secondThenFirst), shader::ShaderTarget::DXIL);

    ASSERT_TRUE(first.Succeeded()) << first.Diagnostics;
    ASSERT_TRUE(second.Succeeded()) << second.Diagnostics;
    const auto firstTexture = first.Value->FindBinding("ShadowTexture");
    const auto secondTexture = second.Value->FindBinding("ShadowTexture");
    ASSERT_TRUE(firstTexture.has_value() && secondTexture.has_value());
    EXPECT_EQ(firstTexture->Record.Binding, 1u);
    EXPECT_EQ(secondTexture->Record.Binding, 2u);
}

TEST(RadRayShaderCompilerClient, MissingIncludeFails) {
    const Client client;
    const VariantInput input{
        .SourceName = "client/shadow.hlsl",
        .Source = ToBytes(kShadowIncludeSource),
        .Targets = shader::ShaderTargetMask::DXIL};

    const auto result = DiscoverAndCompile(client, input, {});

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

TEST(RadRayShaderCompilerClient, EmptyIncludePathListCompilesIncludeFreeSource) {
    const Client client;

    const auto result = DiscoverAndCompile(client, VertexOnly(), {});

    EXPECT_TRUE(result.Succeeded()) << result.Diagnostics;
}

TEST(RadRayShaderCompilerClient, RejectsEmptyIncludePath) {
    const Client client;
    const vector<std::filesystem::path> includePaths{std::filesystem::path{}};

    const auto result = DiscoverAndCompile(client, VertexOnly(), includePaths);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_FALSE(result.Diagnostics.empty());
}

}  // namespace
}  // namespace radray::shader_compiler
