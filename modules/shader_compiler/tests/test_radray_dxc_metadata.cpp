#include <radray/shader_compiler/client.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <optional>

namespace radray::shader_compiler {
namespace {

vector<byte> CopyBytes(std::string_view source) {
    const auto* data = reinterpret_cast<const byte*>(source.data());
    return {data, data + source.size()};
}

vector<std::filesystem::path> ShaderIncludePaths() {
    return {
        std::filesystem::path{RADRAY_PROJECT_DIR} / "shaderlib",
        std::filesystem::path{RADRAY_PROJECT_DIR} / "modules/shader_compiler/tests/data/includes"};
}

constexpr std::string_view kGraphicsSource = R"hlsl(
#include <core/platform.hlsli>
#if defined(USE_EXTRA)
#include <extra.hlsli>
#endif

VK_BINDING(0, 1)
Texture2D<float4> AlbedoTexture : register(t0, space1);
VK_BINDING(1, 1)
SamplerState LinearSampler : register(s0, space1);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain(float2 uv : TEXCOORD0) : SV_Target0 {
#if defined(USE_EXTRA)
    return AlbedoTexture.SampleLevel(LinearSampler, uv, 0.0) + float4(EXTRA_VALUE, 0.0, 0.0, 0.0);
#else
    return AlbedoTexture.SampleLevel(LinearSampler, uv, 0.0);
#endif
}
)hlsl";

constexpr std::string_view kShadowSource = R"hlsl(
#include <shadow.hlsli>

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(SHADOW_VALUE, position.y, position.z, 1.0);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return float4(SHADOW_VALUE, 0.0, 0.0, 1.0);
}
)hlsl";

constexpr std::string_view kMultipleRootConstantsSource = R"hlsl(
[RootSignature("RootConstants(num32BitConstants=16, b0, space=0), RootConstants(num32BitConstants=4, b1, space=0)")]
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}
)hlsl";

constexpr std::string_view kSinglePushConstantSource = R"hlsl(
struct PushData {
    float4 Tint;
};

#include <core/platform.hlsli>
#if defined(__spirv__)
VK_PUSH_CONSTANT ConstantBuffer<PushData> PushConstants;
#endif

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}
)hlsl";

constexpr std::string_view kMultiplePushConstantSource = R"hlsl(
struct FirstData {
    float4 Value;
};
struct SecondData {
    float4 Value;
};

#include <core/platform.hlsli>
#if defined(__spirv__)
VK_PUSH_CONSTANT ConstantBuffer<FirstData> First;
VK_PUSH_CONSTANT ConstantBuffer<SecondData> Second;
#endif

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}
)hlsl";

constexpr std::string_view kMissingStaticSamplerSource = R"hlsl(
Texture2D<float4> ActiveTexture : register(t0);
SamplerState MissingSampler : register(s0);

[RootSignature("DescriptorTable(SRV(t0)), StaticSampler(s1, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT)")]
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return ActiveTexture.Load(int3(0, 0, 0)) + float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return ActiveTexture.SampleLevel(MissingSampler, float2(0.0, 0.0), 0.0);
}
)hlsl";

constexpr std::string_view kDuplicateStaticSamplerSource = R"hlsl(
[RootSignature("StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT), StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT)")]
SamplerState LinearSampler;

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return LinearSampler.SampleLevel(Texture2D<float4>(0), float2(0.0, 0.0), 0.0);
}
)hlsl";

constexpr std::string_view kConflictingStaticSamplerSource = R"hlsl(
[RootSignature("StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT), StaticSampler(s1, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT)")]
SamplerState LinearSampler;

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}
)hlsl";

constexpr std::string_view kMismatchedGraphicsRootSignaturesSource = R"hlsl(
#if !defined(__spirv__)
[RootSignature("CBV(b0)")]
#endif
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

#if !defined(__spirv__)
[RootSignature("CBV(b1)")]
#endif
[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}
)hlsl";

constexpr std::string_view kComputeRootSignatureSource = R"hlsl(
[RootSignature("RootConstants(num32BitConstants=4, b0, space=0)")]
[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID) {
}
)hlsl";

constexpr std::string_view kRootSignatureResourceMismatchSource = R"hlsl(
Texture2D<float4> ActiveTexture : register(t0);

[RootSignature("DescriptorTable(SRV(t0), SRV(t1))")]
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return ActiveTexture.Load(int3(0, 0, 0)) + float4(position, 1.0);
}
)hlsl";

constexpr std::string_view kRootSignatureExactResourceSource = R"hlsl(
Texture2D<float4> ActiveTexture : register(t0);

[RootSignature("DescriptorTable(SRV(t0))")]
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return ActiveTexture.Load(int3(0, 0, 0)) + float4(position, 1.0);
}
)hlsl";

constexpr std::string_view kRootSignatureMissingResourceSource = R"hlsl(
Texture2D<float4> ActiveTexture : register(t0);

[RootSignature("DescriptorTable(SRV(t1))")]
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return ActiveTexture.Load(int3(0, 0, 0)) + float4(position, 1.0);
}
)hlsl";

constexpr std::string_view kStaticSamplerSuccessSource = R"hlsl(
#include <core/platform.hlsli>

VK_BINDING(1, 4)
Texture2D<float4> ColorTexture : register(t0);
VK_BINDING(2, 4)
SamplerState ColorSampler : register(s0);

#if !defined(__spirv__)
[RootSignature("DescriptorTable(SRV(t0)), StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT)")]
#endif
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain(float2 uv : TEXCOORD0) : SV_Target0 {
    return ColorTexture.SampleLevel(ColorSampler, uv, 0.0);
}
)hlsl";

TEST(RadRayDxcMetadata, ConcreteVariantReturnsAtomicTargetLanes) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());

    shader::CompileVariantRequest request{
        .SourceName = "fixtures/metadata_graphics.hlsl",
        .RootSource = CopyBytes(kGraphicsSource),
        .Defines = {},
        .Assignments = {},
        .Targets = shader::ShaderTargetMask::All};
    const DiscoveryResult contract = client.DiscoverSourceContract(
        request.SourceName,
        request.RootSource,
        shader::ShaderTarget::DXIL,
        ShaderIncludePaths());
    ASSERT_TRUE(contract.Succeeded());
    request.ExpectedContract = contract.Contract.Hash;

    const auto includePaths = ShaderIncludePaths();
    const shader::CompileVariantResult result = client.CompileVariant(request, includePaths);
    const shader::CompileVariantResult repeat = client.CompileVariant(request, includePaths);
    ASSERT_EQ(result.Status, shader::CompileStatus::Success)
        << (result.Diagnostics.empty() ? "" : result.Diagnostics.front().Message)
        << "\n"
        << (result.Diagnostics.empty() ? "" : result.Diagnostics.back().Message);
    ASSERT_EQ(repeat.Status, shader::CompileStatus::Success)
        << (repeat.Diagnostics.empty() ? "" : repeat.Diagnostics.back().Message);
    ASSERT_EQ(result.Lanes.size(), 2u);
    ASSERT_EQ(repeat.Lanes.size(), result.Lanes.size());
    for (const shader::CompileTargetLane& lane : result.Lanes) {
        EXPECT_EQ(lane.Stages.size(), 2u);
        EXPECT_FALSE(lane.Bytecode.empty());
        ASSERT_GE(lane.Metadata.size(), sizeof(shader::WireMetadataEnvelope));
        shader::WireMetadataEnvelope envelope{};
        std::memcpy(&envelope, lane.Metadata.data(), sizeof(envelope));
        EXPECT_TRUE(shader::ValidateWireMetadataEnvelope(
            lane.Metadata,
            lane.Target,
            envelope.GpuArtifact));
        EXPECT_EQ(envelope.Contract, request.ExpectedContract);
        EXPECT_NE(envelope.BytecodeDigest, shader::BytecodeHash{});
        EXPECT_NE(envelope.PipelineLayoutDigest, shader::PipelineLayoutHash{});
        EXPECT_EQ(envelope.BindingRecords.Size, 2u * sizeof(shader::WireBindingRecord));
        EXPECT_EQ(envelope.RootSignature.Size, 0u);
    }
    for (size_t index = 0; index < result.Lanes.size(); ++index) {
        EXPECT_EQ(result.Lanes[index].Target, repeat.Lanes[index].Target);
        EXPECT_EQ(result.Lanes[index].Bytecode, repeat.Lanes[index].Bytecode);
        EXPECT_EQ(result.Lanes[index].Metadata, repeat.Lanes[index].Metadata);
        ASSERT_EQ(result.Lanes[index].Stages.size(), repeat.Lanes[index].Stages.size());
        for (size_t stageIndex = 0; stageIndex < result.Lanes[index].Stages.size(); ++stageIndex) {
            EXPECT_EQ(
                result.Lanes[index].Stages[stageIndex].Bytecode,
                repeat.Lanes[index].Stages[stageIndex].Bytecode);
        }
    }
}

TEST(RadRayDxcMetadata, CrossTargetDiscoveryComparesFrontendContracts) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();

    shader::SourceContractRequest matchingRequest{
        .SourceName = "fixtures/cross_target_matching.hlsl",
        .RootSource = CopyBytes(kGraphicsSource),
        .Defines = {},
        .Targets = shader::ShaderTargetMask::All,
        .Policy = {}};
    const auto matching = client.DiscoverSourceContract(matchingRequest, includePaths);
    ASSERT_TRUE(matching.Succeeded())
        << (matching.Diagnostics.empty() ? "" : matching.Diagnostics.back().Message);

    constexpr std::string_view divergentSource = R"hlsl(
#if defined(__spirv__)
#define PIXEL_ENTRY PSMainSpirv
#else
#define PIXEL_ENTRY PSMainDxil
#endif

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PIXEL_ENTRY() : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}
)hlsl";
    shader::SourceContractRequest divergentRequest{
        .SourceName = "fixtures/cross_target_divergent.hlsl",
        .RootSource = CopyBytes(divergentSource),
        .Defines = {},
        .Targets = shader::ShaderTargetMask::All,
        .Policy = {}};
    const auto divergent = client.DiscoverSourceContract(divergentRequest, includePaths);
    EXPECT_EQ(divergent.Status, shader::CompileStatus::InvalidRequest);
    const bool hasCrossTargetDiagnostic = std::any_of(
        divergent.Diagnostics.begin(),
        divergent.Diagnostics.end(),
        [](const shader::CompileDiagnostic& diagnostic) noexcept {
            return diagnostic.Code == 2011;
        });
    EXPECT_TRUE(hasCrossTargetDiagnostic);
}

TEST(RadRayDxcMetadata, FilesystemIncludesAreResolvedByDxc) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());

    shader::CompileVariantRequest request{
        .SourceName = "fixtures/metadata_graphics.hlsl",
        .RootSource = CopyBytes(kGraphicsSource),
        .Defines = {},
        .Assignments = {},
        .Targets = shader::ShaderTargetMask::DXIL};
    const DiscoveryResult contract = client.DiscoverSourceContract(
        request.SourceName,
        request.RootSource,
        shader::ShaderTarget::DXIL,
        ShaderIncludePaths());
    ASSERT_TRUE(contract.Succeeded());
    request.ExpectedContract = contract.Contract.Hash;

    const auto includePaths = ShaderIncludePaths();
    const shader::CompileVariantResult first = client.CompileVariant(request, includePaths);
    const shader::CompileVariantResult second = client.CompileVariant(request, includePaths);
    ASSERT_EQ(first.Status, shader::CompileStatus::Success)
        << (first.Diagnostics.empty() ? "" : first.Diagnostics.back().Message);
    ASSERT_EQ(second.Status, shader::CompileStatus::Success)
        << (second.Diagnostics.empty() ? "" : second.Diagnostics.back().Message);
    ASSERT_EQ(first.Lanes.size(), 1u);
    ASSERT_EQ(second.Lanes.size(), 1u);
    EXPECT_EQ(first.Lanes[0].Bytecode, second.Lanes[0].Bytecode);
    EXPECT_EQ(first.Lanes[0].Metadata, second.Lanes[0].Metadata);

    shader::WireMetadataEnvelope firstEnvelope{};
    std::memcpy(&firstEnvelope, first.Lanes[0].Metadata.data(), sizeof(firstEnvelope));
    request.Defines.push_back({"USE_EXTRA", "1"});
    const shader::CompileVariantResult conditional = client.CompileVariant(request, includePaths);
    ASSERT_EQ(conditional.Status, shader::CompileStatus::Success)
        << (conditional.Diagnostics.empty() ? "" : conditional.Diagnostics.back().Message);
    shader::WireMetadataEnvelope conditionalEnvelope{};
    std::memcpy(&conditionalEnvelope, conditional.Lanes[0].Metadata.data(), sizeof(conditionalEnvelope));
    EXPECT_NE(firstEnvelope.BytecodeDigest, conditionalEnvelope.BytecodeDigest);
}

TEST(RadRayDxcMetadata, EmptyIncludePathListIsValidForIncludeFreeRoot) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/no_include.hlsl",
        CopyBytes(kMultipleRootConstantsSource),
        shader::ShaderTarget::DXIL,
        std::span<const std::filesystem::path>{});
    ASSERT_TRUE(discovery.Succeeded());
    const auto result = client.CompileVariant(
        shader::CompileVariantRequest{
            .SourceName = "fixtures/no_include.hlsl",
            .RootSource = CopyBytes(kMultipleRootConstantsSource),
            .Defines = {},
            .Assignments = {},
            .Targets = shader::ShaderTargetMask::DXIL,
            .Policy = {},
            .ExpectedContract = discovery.Contract.Hash},
        std::span<const std::filesystem::path>{});
    EXPECT_EQ(result.Status, shader::CompileStatus::Success)
        << (result.Diagnostics.empty() ? "" : result.Diagnostics.back().Message);
}

TEST(RadRayDxcMetadata, EmptyIncludeDirectoryIsRejected) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const vector<std::filesystem::path> includePaths{std::filesystem::path{}};
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/no_include.hlsl",
        CopyBytes(kMultipleRootConstantsSource),
        shader::ShaderTarget::DXIL,
        includePaths);
    EXPECT_FALSE(discovery.Succeeded());
    EXPECT_FALSE(discovery.Diagnostics.empty());
}

TEST(RadRayDxcMetadata, IncludePathOrderFollowsDxcShadowing) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const std::filesystem::path root{RADRAY_PROJECT_DIR};
    const vector<std::filesystem::path> firstPaths{
        root / "modules/shader_compiler/tests/data/includes/first",
        root / "modules/shader_compiler/tests/data/includes/second"};
    const vector<std::filesystem::path> secondPaths{
        root / "modules/shader_compiler/tests/data/includes/second",
        root / "modules/shader_compiler/tests/data/includes/first"};
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/shadow.hlsl",
        CopyBytes(kShadowSource),
        shader::ShaderTarget::DXIL,
        firstPaths);
    ASSERT_TRUE(discovery.Succeeded());
    const shader::CompileVariantRequest request{
        .SourceName = "fixtures/shadow.hlsl",
        .RootSource = CopyBytes(kShadowSource),
        .Defines = {},
        .Assignments = {},
        .Targets = shader::ShaderTargetMask::DXIL,
        .Policy = {},
        .ExpectedContract = discovery.Contract.Hash};
    const auto first = client.CompileVariant(request, firstPaths);
    const auto second = client.CompileVariant(request, secondPaths);
    ASSERT_EQ(first.Status, shader::CompileStatus::Success);
    ASSERT_EQ(second.Status, shader::CompileStatus::Success);
    ASSERT_EQ(first.Lanes.size(), 1u);
    ASSERT_EQ(second.Lanes.size(), 1u);
    shader::WireMetadataEnvelope firstEnvelope{};
    shader::WireMetadataEnvelope secondEnvelope{};
    std::memcpy(&firstEnvelope, first.Lanes.front().Metadata.data(), sizeof(firstEnvelope));
    std::memcpy(&secondEnvelope, second.Lanes.front().Metadata.data(), sizeof(secondEnvelope));
    EXPECT_NE(firstEnvelope.BytecodeDigest, secondEnvelope.BytecodeDigest);
}

TEST(RadRayDxcMetadata, RootAndPushConstantMetadataFacts) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());

    const auto compile = [&](std::string_view sourceName,
                             std::string_view source,
                             shader::ShaderTarget target) {
        const auto includePaths = ShaderIncludePaths();
        const auto discovery = client.DiscoverSourceContract(
            sourceName,
            CopyBytes(source),
            target,
            includePaths);
        EXPECT_TRUE(discovery.Succeeded());
        if (!discovery.Succeeded()) {
            return shader::CompileVariantResult{};
        }
        return client.CompileVariant(shader::CompileVariantRequest{
            .SourceName = string{sourceName},
            .RootSource = CopyBytes(source),
            .Defines = {},
            .Assignments = {},
            .Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(target)),
            .ExpectedContract = discovery.Contract.Hash},
            includePaths);
    };

    const auto rootResult = compile(
        "fixtures/multiple_root_constants.hlsl",
        kMultipleRootConstantsSource,
        shader::ShaderTarget::DXIL);
    ASSERT_EQ(rootResult.Status, shader::CompileStatus::Success)
        << (rootResult.Diagnostics.empty() ? "" : rootResult.Diagnostics.back().Message);
    ASSERT_EQ(rootResult.Lanes.size(), 1u);
    shader::WireMetadataEnvelope rootEnvelope{};
    std::memcpy(&rootEnvelope, rootResult.Lanes[0].Metadata.data(), sizeof(rootEnvelope));
    EXPECT_GT(rootEnvelope.RootSignature.Size, 0u);
    ASSERT_EQ(rootEnvelope.RootConstantRecords.Size, 2u * sizeof(shader::WireRootConstantRecord));
    vector<shader::WireRootConstantRecord> roots(2);
    std::memcpy(
        roots.data(),
        rootResult.Lanes[0].Metadata.data() + rootEnvelope.RootConstantRecords.Offset,
        rootEnvelope.RootConstantRecords.Size);
    EXPECT_EQ(roots[0].RegisterSpace, 0u);
    EXPECT_EQ(roots[0].Register, 0u);
    EXPECT_EQ(roots[0].Size, 64u);
    EXPECT_EQ(roots[0].StageMask, 0x3u);
    EXPECT_EQ(roots[1].RegisterSpace, 0u);
    EXPECT_EQ(roots[1].Register, 1u);
    EXPECT_EQ(roots[1].Size, 16u);
    EXPECT_EQ(roots[1].StageMask, 0x3u);

    const auto pushResult = compile(
        "fixtures/single_push_constant.hlsl",
        kSinglePushConstantSource,
        shader::ShaderTarget::SPIRV);
    ASSERT_EQ(pushResult.Status, shader::CompileStatus::Success)
        << (pushResult.Diagnostics.empty() ? "" : pushResult.Diagnostics.back().Message);
    ASSERT_EQ(pushResult.Lanes.size(), 1u);
    shader::WireMetadataEnvelope pushEnvelope{};
    std::memcpy(&pushEnvelope, pushResult.Lanes[0].Metadata.data(), sizeof(pushEnvelope));
    EXPECT_EQ(pushEnvelope.RootSignature.Size, 0u);
    ASSERT_EQ(pushEnvelope.RootConstantRecords.Size, sizeof(shader::WireRootConstantRecord));
    shader::WireRootConstantRecord push{};
    std::memcpy(
        &push,
        pushResult.Lanes[0].Metadata.data() + pushEnvelope.RootConstantRecords.Offset,
        sizeof(push));
    EXPECT_EQ(push.RegisterSpace, 0u);
    EXPECT_EQ(push.Register, 0u);
    EXPECT_EQ(push.Size, 16u);
    EXPECT_EQ(push.StageMask, 0x3u);
    EXPECT_EQ(push.Flags, 1u);

    const auto multiplePushResult = compile(
        "fixtures/multiple_push_constant.hlsl",
        kMultiplePushConstantSource,
        shader::ShaderTarget::SPIRV);
    EXPECT_EQ(multiplePushResult.Status, shader::CompileStatus::TargetFailure);
    EXPECT_TRUE(multiplePushResult.Lanes.empty());
    EXPECT_FALSE(multiplePushResult.Diagnostics.empty());

    const auto computeResult = compile(
        "fixtures/compute_root_signature.hlsl",
        kComputeRootSignatureSource,
        shader::ShaderTarget::DXIL);
    ASSERT_EQ(computeResult.Status, shader::CompileStatus::Success)
        << (computeResult.Diagnostics.empty() ? "" : computeResult.Diagnostics.back().Message);
    ASSERT_EQ(computeResult.Lanes.size(), 1u);
    shader::WireMetadataEnvelope computeEnvelope{};
    std::memcpy(&computeEnvelope, computeResult.Lanes[0].Metadata.data(), sizeof(computeEnvelope));
    EXPECT_GT(computeEnvelope.RootSignature.Size, 0u);
    ASSERT_EQ(computeEnvelope.RootConstantRecords.Size, sizeof(shader::WireRootConstantRecord));
    shader::WireRootConstantRecord computeRoot{};
    std::memcpy(
        &computeRoot,
        computeResult.Lanes[0].Metadata.data() + computeEnvelope.RootConstantRecords.Offset,
        sizeof(computeRoot));
    EXPECT_EQ(computeRoot.Register, 0u);
    EXPECT_EQ(computeRoot.Size, 16u);
    EXPECT_EQ(computeRoot.StageMask, 0x4u);
}

TEST(RadRayDxcMetadata, StaticSamplerPolicyFailsClosed) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto expectTargetFailure = [&](std::string_view name, std::string_view source) {
        const auto discovery = client.DiscoverSourceContract(
            name,
            CopyBytes(source),
            shader::ShaderTarget::DXIL,
            includePaths);
        ASSERT_TRUE(discovery.Succeeded());
        const auto result = client.CompileVariant(shader::CompileVariantRequest{
            .SourceName = string{name},
            .RootSource = CopyBytes(source),
            .Defines = {},
            .Assignments = {},
            .Targets = shader::ShaderTargetMask::DXIL,
            .ExpectedContract = discovery.Contract.Hash},
            includePaths);
        EXPECT_EQ(result.Status, shader::CompileStatus::TargetFailure) << name;
        EXPECT_TRUE(result.Lanes.empty()) << name;
        EXPECT_FALSE(result.Diagnostics.empty()) << name;
    };
    const auto expectDiscoveryFailure = [&](std::string_view name, std::string_view source) {
        const auto discovery = client.DiscoverSourceContract(
            name,
            CopyBytes(source),
            shader::ShaderTarget::DXIL,
            includePaths);
        EXPECT_FALSE(discovery.Succeeded()) << name;
        EXPECT_EQ(discovery.Status, shader::CompileStatus::InvalidRequest) << name;
        EXPECT_FALSE(discovery.Diagnostics.empty()) << name;
    };
    expectTargetFailure("fixtures/missing_static_sampler.hlsl", kMissingStaticSamplerSource);
    expectDiscoveryFailure("fixtures/duplicate_static_sampler.hlsl", kDuplicateStaticSamplerSource);
    expectDiscoveryFailure("fixtures/conflicting_static_sampler.hlsl", kConflictingStaticSamplerSource);
}

TEST(RadRayDxcMetadata, ExplicitRootSignatureAllowsStableSuperset) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/root_signature_resource_mismatch.hlsl",
        CopyBytes(kRootSignatureResourceMismatchSource),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded());
    const auto result = client.CompileVariant(shader::CompileVariantRequest{
        .SourceName = "fixtures/root_signature_resource_mismatch.hlsl",
        .RootSource = CopyBytes(kRootSignatureResourceMismatchSource),
        .Defines = {},
        .Assignments = {},
        .Targets = shader::ShaderTargetMask::DXIL,
        .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    ASSERT_EQ(result.Status, shader::CompileStatus::Success)
        << (result.Diagnostics.empty() ? "" : result.Diagnostics.back().Message);
    ASSERT_EQ(result.Lanes.size(), 1u);
    shader::WireMetadataEnvelope envelope{};
    std::memcpy(&envelope, result.Lanes[0].Metadata.data(), sizeof(envelope));
    EXPECT_GT(envelope.RootSignature.Size, 0u);
    const byte rts0[] = {
        static_cast<byte>('R'), static_cast<byte>('T'),
        static_cast<byte>('S'), static_cast<byte>('0')};
    EXPECT_EQ(
        std::search(
            result.Lanes[0].Bytecode.begin(),
            result.Lanes[0].Bytecode.end(),
            rts0,
            rts0 + 4),
        result.Lanes[0].Bytecode.end());

    const auto exactDiscovery = client.DiscoverSourceContract(
        "fixtures/root_signature_exact_resource.hlsl",
        CopyBytes(kRootSignatureExactResourceSource),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(exactDiscovery.Succeeded());
    const auto exactResult = client.CompileVariant(shader::CompileVariantRequest{
        .SourceName = "fixtures/root_signature_exact_resource.hlsl",
        .RootSource = CopyBytes(kRootSignatureExactResourceSource),
        .Defines = {},
        .Assignments = {},
        .Targets = shader::ShaderTargetMask::DXIL,
        .ExpectedContract = exactDiscovery.Contract.Hash},
        includePaths);
    ASSERT_EQ(exactResult.Status, shader::CompileStatus::Success)
        << (exactResult.Diagnostics.empty() ? "" : exactResult.Diagnostics.back().Message);
    ASSERT_EQ(exactResult.Lanes.size(), 1u);
    shader::WireMetadataEnvelope exactEnvelope{};
    std::memcpy(&exactEnvelope, exactResult.Lanes[0].Metadata.data(), sizeof(exactEnvelope));
    EXPECT_NE(exactEnvelope.PipelineLayoutDigest, envelope.PipelineLayoutDigest);
    EXPECT_NE(exactEnvelope.GpuArtifact, envelope.GpuArtifact);
}

TEST(RadRayDxcMetadata, ExplicitRootSignatureMustCoverActiveResource) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/root_signature_missing_resource.hlsl",
        CopyBytes(kRootSignatureMissingResourceSource),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded());
    const auto result = client.CompileVariant(shader::CompileVariantRequest{
        .SourceName = "fixtures/root_signature_missing_resource.hlsl",
        .RootSource = CopyBytes(kRootSignatureMissingResourceSource),
        .Defines = {},
        .Assignments = {},
        .Targets = shader::ShaderTargetMask::DXIL,
        .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    EXPECT_EQ(result.Status, shader::CompileStatus::TargetFailure);
    EXPECT_TRUE(result.Lanes.empty());
    const bool hasRootDiagnostic = std::any_of(
        result.Diagnostics.begin(),
        result.Diagnostics.end(),
        [](const shader::CompileDiagnostic& diagnostic) noexcept {
            return diagnostic.Code == 2106;
        });
    EXPECT_TRUE(hasRootDiagnostic);
}

TEST(RadRayDxcMetadata, StaticSamplerIsTargetSpecificButImmutable) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/static_sampler_success.hlsl",
        CopyBytes(kStaticSamplerSuccessSource),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded());
    const auto result = client.CompileVariant(shader::CompileVariantRequest{
        .SourceName = "fixtures/static_sampler_success.hlsl",
        .RootSource = CopyBytes(kStaticSamplerSuccessSource),
        .Defines = {},
        .Assignments = {},
        .Targets = shader::ShaderTargetMask::All,
        .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    ASSERT_EQ(result.Status, shader::CompileStatus::Success)
        << (result.Diagnostics.empty() ? "" : result.Diagnostics.back().Message);
    ASSERT_EQ(result.Lanes.size(), 2u);

    uint32_t dxilTextureBinding = 0;
    uint32_t spirvTextureBinding = 0;
    for (const shader::CompileTargetLane& lane : result.Lanes) {
        shader::WireMetadataEnvelope envelope{};
        ASSERT_GE(lane.Metadata.size(), sizeof(envelope));
        std::memcpy(&envelope, lane.Metadata.data(), sizeof(envelope));
        if (lane.Target == shader::ShaderTarget::DXIL) {
            EXPECT_GT(envelope.RootSignature.Size, 0u);
        } else {
            EXPECT_EQ(envelope.RootSignature.Size, 0u);
        }
        vector<shader::WireBindingRecord> bindings(
            envelope.BindingRecords.Size / sizeof(shader::WireBindingRecord));
        std::memcpy(
            bindings.data(),
            lane.Metadata.data() + envelope.BindingRecords.Offset,
            envelope.BindingRecords.Size);
        bool foundTexture = false;
        bool foundSampler = false;
        for (const shader::WireBindingRecord& binding : bindings) {
            const std::string_view name{
                reinterpret_cast<const char*>(lane.Metadata.data() + binding.Name.Offset),
                binding.Name.Size};
            if (name == "ColorTexture") {
                foundTexture = true;
                if (lane.Target == shader::ShaderTarget::DXIL) {
                    dxilTextureBinding = binding.Binding;
                    EXPECT_EQ(binding.Group, 0u);
                } else {
                    spirvTextureBinding = binding.Binding;
                    EXPECT_EQ(binding.Group, 4u);
                }
            }
            if (name == "ColorSampler") {
                foundSampler = true;
                EXPECT_NE(binding.Flags & 1u, 0u)
                    << "target=" << static_cast<uint32_t>(lane.Target);
            }
        }
        EXPECT_TRUE(foundTexture);
        EXPECT_TRUE(foundSampler);
    }
    EXPECT_NE(dxilTextureBinding, spirvTextureBinding);
}

TEST(RadRayDxcMetadata, MismatchedGraphicsRootSignaturesFailClosed) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/mismatched_graphics_root_signatures.hlsl",
        CopyBytes(kMismatchedGraphicsRootSignaturesSource),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded());
    for (const shader::ShaderTarget target :
         {shader::ShaderTarget::DXIL, shader::ShaderTarget::SPIRV}) {
        const auto result = client.CompileVariant(shader::CompileVariantRequest{
            .SourceName = "fixtures/mismatched_graphics_root_signatures.hlsl",
            .RootSource = CopyBytes(kMismatchedGraphicsRootSignaturesSource),
            .Defines = {},
            .Assignments = {},
            .Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(target)),
            .ExpectedContract = discovery.Contract.Hash},
            includePaths);
        EXPECT_EQ(result.Status, shader::CompileStatus::TargetFailure)
            << "target=" << static_cast<uint32_t>(target);
        EXPECT_TRUE(result.Lanes.empty())
            << "target=" << static_cast<uint32_t>(target);
        const bool hasRootDiagnostic = std::any_of(
            result.Diagnostics.begin(),
            result.Diagnostics.end(),
            [](const shader::CompileDiagnostic& diagnostic) noexcept {
                return diagnostic.Code == 2105;
            });
        EXPECT_TRUE(hasRootDiagnostic)
            << "target=" << static_cast<uint32_t>(target);
    }
}

TEST(RadRayDxcMetadata, ContractDriftFailsClosed) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());

    shader::CompileVariantRequest request{
        .SourceName = "fixtures/metadata_graphics.hlsl",
        .RootSource = CopyBytes(kGraphicsSource),
        .Defines = {},
        .Assignments = {},
        .Targets = shader::ShaderTargetMask::DXIL};
    request.ExpectedContract.Bytes.fill(0xff);
    const shader::CompileVariantResult result = client.CompileVariant(request, ShaderIncludePaths());
    EXPECT_EQ(result.Status, shader::CompileStatus::ContractMismatch);
    EXPECT_TRUE(result.Lanes.empty());
}

TEST(RadRayDxcMetadata, AssignmentInducedTopologyDriftIsContractMismatch) {
    constexpr std::string_view source = R"hlsl(
#pragma radray_keyword_group PIXEL_ENTRY "PSLow" "PSHigh"

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PIXEL_ENTRY() : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}
)hlsl";

    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/assignment_topology_drift.hlsl",
        CopyBytes(source),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded())
        << (discovery.Diagnostics.empty() ? "" : discovery.Diagnostics.back().Message);

    const auto result = client.CompileVariant(
        shader::CompileVariantRequest{
            .SourceName = "fixtures/assignment_topology_drift.hlsl",
            .RootSource = CopyBytes(source),
            .Defines = {},
            .Assignments = {{string{"PIXEL_ENTRY"}, string{"PSLow"}}},
            .Targets = shader::ShaderTargetMask::DXIL,
            .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    EXPECT_EQ(result.Status, shader::CompileStatus::ContractMismatch);
    EXPECT_TRUE(result.Lanes.empty());
    const bool hasTopologyDiagnostic = std::any_of(
        result.Diagnostics.begin(),
        result.Diagnostics.end(),
        [](const shader::CompileDiagnostic& diagnostic) noexcept {
            return diagnostic.Code == 2007;
        });
    EXPECT_TRUE(hasTopologyDiagnostic);
}

// Regression: the frontend collects per stage, and each stage only keeps the
// resources it actually uses. A merge that demanded identical resource sets
// across stages reported 2107 for every shader shaped like this one.
constexpr std::string_view kStageDisjointResourceSource = R"hlsl(
#include <core/platform.hlsli>

struct VertexData {
    float4x4 Transform;
};
struct PixelData {
    float4 Tint;
};

VK_BINDING(0, 0)
ConstantBuffer<VertexData> VertexConstants : register(b0);
VK_BINDING(1, 0)
ConstantBuffer<PixelData> PixelConstants : register(b1);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return mul(VertexConstants.Transform, float4(position, 1.0));
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return PixelConstants.Tint;
}
)hlsl";

TEST(RadRayDxcMetadata, StagesMayUseDisjointResourceSets) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/stage_disjoint_resources.hlsl",
        CopyBytes(kStageDisjointResourceSource),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded())
        << (discovery.Diagnostics.empty() ? "" : discovery.Diagnostics.back().Message);
    const auto result = client.CompileVariant(
        shader::CompileVariantRequest{
            .SourceName = "fixtures/stage_disjoint_resources.hlsl",
            .RootSource = CopyBytes(kStageDisjointResourceSource),
            .Defines = {},
            .Assignments = {},
            .Targets = shader::ShaderTargetMask::All,
            .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    ASSERT_EQ(result.Status, shader::CompileStatus::Success)
        << (result.Diagnostics.empty() ? "" : result.Diagnostics.back().Message);
    ASSERT_EQ(result.Lanes.size(), 2u);

    // Both lanes must carry the union of the two stages' resources, each tagged
    // with only the stage that uses it.
    for (const shader::CompileTargetLane& lane : result.Lanes) {
        shader::WireMetadataEnvelope envelope{};
        ASSERT_GE(lane.Metadata.size(), sizeof(envelope));
        std::memcpy(&envelope, lane.Metadata.data(), sizeof(envelope));
        vector<shader::WireBindingRecord> bindings(
            envelope.BindingRecords.Size / sizeof(shader::WireBindingRecord));
        ASSERT_EQ(bindings.size(), 2u)
            << "target=" << static_cast<uint32_t>(lane.Target);
        std::memcpy(
            bindings.data(),
            lane.Metadata.data() + envelope.BindingRecords.Offset,
            envelope.BindingRecords.Size);
        for (const shader::WireBindingRecord& binding : bindings) {
            const std::string_view name{
                reinterpret_cast<const char*>(lane.Metadata.data() + binding.Name.Offset),
                binding.Name.Size};
            const uint32_t expectedStage =
                1u << static_cast<uint8_t>(
                    name == "VertexConstants" ? shader::ShaderStage::Vertex
                                              : shader::ShaderStage::Pixel);
            EXPECT_EQ(binding.StageMask, expectedStage) << name;
        }
    }
}

// Regression: vertex input locations must come from the emitted SPIR-V module.
// An AST-order counter silently disagreed with the DXIL input signature once a
// struct parameter was involved, which surfaced as 2108 on merge.
constexpr std::string_view kStructVertexInputSource = R"hlsl(
struct VertexInput {
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
};

[shader("vertex")]
float4 VSMain(VertexInput input) : SV_Position {
    return float4(input.Position + input.Normal, input.UV.x);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}
)hlsl";

TEST(RadRayDxcMetadata, StructVertexInputsAgreeAcrossTargets) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/struct_vertex_input.hlsl",
        CopyBytes(kStructVertexInputSource),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded())
        << (discovery.Diagnostics.empty() ? "" : discovery.Diagnostics.back().Message);
    const auto result = client.CompileVariant(
        shader::CompileVariantRequest{
            .SourceName = "fixtures/struct_vertex_input.hlsl",
            .RootSource = CopyBytes(kStructVertexInputSource),
            .Defines = {},
            .Assignments = {},
            .Targets = shader::ShaderTargetMask::All,
            .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    ASSERT_EQ(result.Status, shader::CompileStatus::Success)
        << (result.Diagnostics.empty() ? "" : result.Diagnostics.back().Message);
    ASSERT_EQ(result.Lanes.size(), 2u);

    struct ExpectedInput {
        std::string_view Semantic;
        uint32_t Location;
        uint32_t ComponentCount;
    };
    constexpr ExpectedInput expected[] = {
        {"POSITION", 0, 3},
        {"NORMAL", 1, 3},
        {"TEXCOORD", 2, 2}};

    for (const shader::CompileTargetLane& lane : result.Lanes) {
        shader::WireMetadataEnvelope envelope{};
        ASSERT_GE(lane.Metadata.size(), sizeof(envelope));
        std::memcpy(&envelope, lane.Metadata.data(), sizeof(envelope));
        vector<shader::WireVertexInputRecord> inputs(
            envelope.VertexInputRecords.Size / sizeof(shader::WireVertexInputRecord));
        ASSERT_EQ(inputs.size(), std::size(expected))
            << "target=" << static_cast<uint32_t>(lane.Target);
        std::memcpy(
            inputs.data(),
            lane.Metadata.data() + envelope.VertexInputRecords.Offset,
            envelope.VertexInputRecords.Size);
        for (const ExpectedInput& want : expected) {
            const auto found = std::find_if(
                inputs.begin(),
                inputs.end(),
                [&](const shader::WireVertexInputRecord& record) noexcept {
                    const std::string_view name{
                        reinterpret_cast<const char*>(
                            lane.Metadata.data() + record.Semantic.Offset),
                        record.Semantic.Size};
                    return name == want.Semantic;
                });
            ASSERT_NE(found, inputs.end())
                << want.Semantic << " target=" << static_cast<uint32_t>(lane.Target);
            EXPECT_EQ(found->Location, want.Location) << want.Semantic;
            EXPECT_EQ(found->ComponentCount, want.ComponentCount) << want.Semantic;
        }
    }
}

TEST(RadRayDxcMetadata, MatrixVertexInputExpandsToLocationRows) {
    constexpr std::string_view source = R"hlsl(
struct VertexInput {
    float3 Position : POSITION;
    float3x3 Basis : TEXCOORD0;
};

[shader("vertex")]
float4 VSMain(VertexInput input) : SV_Position {
    return float4(mul(input.Position, input.Basis), 1.0);
}
)hlsl";

    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/matrix_vertex_input.hlsl",
        CopyBytes(source),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded())
        << (discovery.Diagnostics.empty() ? "" : discovery.Diagnostics.back().Message);
    const auto result = client.CompileVariant(
        shader::CompileVariantRequest{
            .SourceName = "fixtures/matrix_vertex_input.hlsl",
            .RootSource = CopyBytes(source),
            .Defines = {},
            .Assignments = {},
            .Targets = shader::ShaderTargetMask::All,
            .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    ASSERT_EQ(result.Status, shader::CompileStatus::Success)
        << (result.Diagnostics.empty() ? "" : result.Diagnostics.back().Message);
    ASSERT_EQ(result.Lanes.size(), 2u);

    struct ExpectedInput {
        std::string_view Semantic;
        uint32_t SemanticIndex;
        uint32_t Location;
    };
    constexpr ExpectedInput expected[] = {
        {"POSITION", 0, 0},
        {"TEXCOORD", 0, 1},
        {"TEXCOORD", 1, 2},
        {"TEXCOORD", 2, 3}};

    for (const shader::CompileTargetLane& lane : result.Lanes) {
        shader::WireMetadataEnvelope envelope{};
        std::memcpy(&envelope, lane.Metadata.data(), sizeof(envelope));
        vector<shader::WireVertexInputRecord> inputs(
            envelope.VertexInputRecords.Size / sizeof(shader::WireVertexInputRecord));
        ASSERT_EQ(inputs.size(), std::size(expected))
            << "target=" << static_cast<uint32_t>(lane.Target);
        std::memcpy(
            inputs.data(),
            lane.Metadata.data() + envelope.VertexInputRecords.Offset,
            envelope.VertexInputRecords.Size);
        for (const ExpectedInput& want : expected) {
            const auto found = std::find_if(
                inputs.begin(),
                inputs.end(),
                [&](const shader::WireVertexInputRecord& record) noexcept {
                    const std::string_view name{
                        reinterpret_cast<const char*>(
                            lane.Metadata.data() + record.Semantic.Offset),
                        record.Semantic.Size};
                    return name == want.Semantic &&
                           record.SemanticIndex == want.SemanticIndex;
                });
            ASSERT_NE(found, inputs.end())
                << want.Semantic << want.SemanticIndex
                << " target=" << static_cast<uint32_t>(lane.Target);
            EXPECT_EQ(found->Location, want.Location);
            EXPECT_EQ(found->ComponentCount, 3u);
        }
    }
}

TEST(RadRayDxcMetadata, SpirvMergeRejectsCrossStageDxilRegisterDrift) {
    constexpr std::string_view source = R"hlsl(
#include <core/platform.hlsli>

#if __SHADER_TARGET_STAGE == __SHADER_STAGE_VERTEX
#define SHARED_REGISTER t0
#else
#define SHARED_REGISTER t1
#endif

VK_BINDING(0, 0)
Texture2D<float4> SharedTexture : register(SHARED_REGISTER);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return SharedTexture.Load(int3(0, 0, 0)) + float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return SharedTexture.Load(int3(0, 0, 0));
}
)hlsl";

    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/cross_stage_register_drift.hlsl",
        CopyBytes(source),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded())
        << (discovery.Diagnostics.empty() ? "" : discovery.Diagnostics.back().Message);
    const auto result = client.CompileVariant(
        shader::CompileVariantRequest{
            .SourceName = "fixtures/cross_stage_register_drift.hlsl",
            .RootSource = CopyBytes(source),
            .Defines = {},
            .Assignments = {},
            .Targets = shader::ShaderTargetMask::SPIRV,
            .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    EXPECT_EQ(result.Status, shader::CompileStatus::TargetFailure);
    EXPECT_TRUE(result.Lanes.empty());
    const bool hasBindingDiagnostic = std::any_of(
        result.Diagnostics.begin(),
        result.Diagnostics.end(),
        [](const shader::CompileDiagnostic& diagnostic) noexcept {
            return diagnostic.Code == 2109;
        });
    EXPECT_TRUE(hasBindingDiagnostic);
}

TEST(RadRayDxcMetadata, ResourceArrayPreservesCountAcrossTargets) {
    constexpr std::string_view source = R"hlsl(
#include <core/platform.hlsli>

VK_BINDING(4, 1)
Texture2D<float4> Textures[2] : register(t3, space2);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return Textures[0].Load(int3(0, 0, 0)) + float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return Textures[1].Load(int3(0, 0, 0));
}
)hlsl";

    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/resource_array.hlsl",
        CopyBytes(source),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded())
        << (discovery.Diagnostics.empty() ? "" : discovery.Diagnostics.back().Message);
    const auto result = client.CompileVariant(
        shader::CompileVariantRequest{
            .SourceName = "fixtures/resource_array.hlsl",
            .RootSource = CopyBytes(source),
            .Defines = {},
            .Assignments = {},
            .Targets = shader::ShaderTargetMask::All,
            .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    ASSERT_EQ(result.Status, shader::CompileStatus::Success)
        << (result.Diagnostics.empty() ? "" : result.Diagnostics.back().Message);
    ASSERT_EQ(result.Lanes.size(), 2u);
    for (const shader::CompileTargetLane& lane : result.Lanes) {
        shader::WireMetadataEnvelope envelope{};
        std::memcpy(&envelope, lane.Metadata.data(), sizeof(envelope));
        ASSERT_EQ(envelope.BindingRecords.Size, sizeof(shader::WireBindingRecord));
        shader::WireBindingRecord binding{};
        std::memcpy(
            &binding,
            lane.Metadata.data() + envelope.BindingRecords.Offset,
            sizeof(binding));
        EXPECT_EQ(binding.Count, 2u)
            << "target=" << static_cast<uint32_t>(lane.Target);
        EXPECT_EQ(binding.StageMask, 0x3u)
            << "target=" << static_cast<uint32_t>(lane.Target);
    }
}

// An implicit binding is invisible in the emitted artifact: DXC assigns a slot
// either way. Each lane must reject its own missing annotation at compile time.
TEST(RadRayDxcMetadata, ImplicitBindingIsRejectedPerLane) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto expectLaneFailure = [&](std::string_view name,
                                       std::string_view source,
                                       shader::ShaderTarget target,
                                       uint32_t expectedCode) {
        const auto discovery = client.DiscoverSourceContract(
            name,
            CopyBytes(source),
            target,
            includePaths);
        ASSERT_TRUE(discovery.Succeeded()) << name;
        const auto result = client.CompileVariant(
            shader::CompileVariantRequest{
                .SourceName = string{name},
                .RootSource = CopyBytes(source),
                .Defines = {},
                .Assignments = {},
                .Targets = static_cast<shader::ShaderTargetMask>(
                    shader::ToTargetMask(target)),
                .ExpectedContract = discovery.Contract.Hash},
            includePaths);
        EXPECT_EQ(result.Status, shader::CompileStatus::TargetFailure) << name;
        EXPECT_TRUE(result.Lanes.empty()) << name;
        const bool hasExpectedCode = std::any_of(
            result.Diagnostics.begin(),
            result.Diagnostics.end(),
            [&](const shader::CompileDiagnostic& diagnostic) noexcept {
                return diagnostic.Code == expectedCode;
            });
        EXPECT_TRUE(hasExpectedCode)
            << name << " expected diagnostic " << expectedCode;
    };

    // Carries [[vk::binding]] but no register(): the DXIL lane must refuse it.
    constexpr std::string_view missingRegisterSource = R"hlsl(
#include <core/platform.hlsli>

VK_BINDING(3, 0)
Texture2D<float4> ImplicitInDxil;

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return ImplicitInDxil.Load(int3(0, 0, 0)) + float4(position, 1.0);
}
)hlsl";
    expectLaneFailure(
        "fixtures/missing_register.hlsl",
        missingRegisterSource,
        shader::ShaderTarget::DXIL,
        2111);

    // Array wrappers must not hide the resource type from the AST-side check.
    constexpr std::string_view missingArrayRegisterSource = R"hlsl(
#include <core/platform.hlsli>

VK_BINDING(3, 0)
Texture2D<float4> ImplicitArrayInDxil[2];

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return ImplicitArrayInDxil[1].Load(int3(0, 0, 0)) + float4(position, 1.0);
}
)hlsl";
    expectLaneFailure(
        "fixtures/missing_array_register.hlsl",
        missingArrayRegisterSource,
        shader::ShaderTarget::DXIL,
        2111);

    // Carries register() but no [[vk::binding]]: the SPIR-V lane must refuse it.
    constexpr std::string_view missingVkBindingSource = R"hlsl(
Texture2D<float4> ImplicitInSpirv : register(t4);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return ImplicitInSpirv.Load(int3(0, 0, 0)) + float4(position, 1.0);
}
)hlsl";
    expectLaneFailure(
        "fixtures/missing_vk_binding.hlsl",
        missingVkBindingSource,
        shader::ShaderTarget::SPIRV,
        2113);

    // Binding enforcement happens after dead-resource removal. An entirely
    // unused declaration is not part of either lane's artifact contract.
    constexpr std::string_view deadImplicitResourceSource = R"hlsl(
Texture2D<float4> DeadTexture;

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}
)hlsl";
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/dead_implicit_resource.hlsl",
        CopyBytes(deadImplicitResourceSource),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded());
    const auto deadResult = client.CompileVariant(
        shader::CompileVariantRequest{
            .SourceName = "fixtures/dead_implicit_resource.hlsl",
            .RootSource = CopyBytes(deadImplicitResourceSource),
            .Defines = {},
            .Assignments = {},
            .Targets = shader::ShaderTargetMask::All,
            .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    ASSERT_EQ(deadResult.Status, shader::CompileStatus::Success)
        << (deadResult.Diagnostics.empty() ? "" : deadResult.Diagnostics.back().Message);
    ASSERT_EQ(deadResult.Lanes.size(), 2u);
    for (const shader::CompileTargetLane& lane : deadResult.Lanes) {
        shader::WireMetadataEnvelope envelope{};
        std::memcpy(&envelope, lane.Metadata.data(), sizeof(envelope));
        EXPECT_EQ(envelope.BindingRecords.Size, 0u);
    }
}

// Regression: RootConstants inherited an all-stage mask regardless of the
// author's ShaderVisibility, so a vertex-only constant claimed the pixel stage.
constexpr std::string_view kVisibilityScopedRootConstantSource = R"hlsl(
[RootSignature("RootConstants(num32BitConstants=4, b0, space=0, visibility=SHADER_VISIBILITY_VERTEX)")]
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}
)hlsl";

TEST(RadRayDxcMetadata, RootConstantStageMaskFollowsAuthoredVisibility) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/visibility_scoped_root_constant.hlsl",
        CopyBytes(kVisibilityScopedRootConstantSource),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded())
        << (discovery.Diagnostics.empty() ? "" : discovery.Diagnostics.back().Message);
    const auto result = client.CompileVariant(
        shader::CompileVariantRequest{
            .SourceName = "fixtures/visibility_scoped_root_constant.hlsl",
            .RootSource = CopyBytes(kVisibilityScopedRootConstantSource),
            .Defines = {},
            .Assignments = {},
            .Targets = shader::ShaderTargetMask::DXIL,
            .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    ASSERT_EQ(result.Status, shader::CompileStatus::Success)
        << (result.Diagnostics.empty() ? "" : result.Diagnostics.back().Message);
    ASSERT_EQ(result.Lanes.size(), 1u);
    shader::WireMetadataEnvelope envelope{};
    std::memcpy(&envelope, result.Lanes[0].Metadata.data(), sizeof(envelope));
    ASSERT_EQ(envelope.RootConstantRecords.Size, sizeof(shader::WireRootConstantRecord));
    shader::WireRootConstantRecord root{};
    std::memcpy(
        &root,
        result.Lanes[0].Metadata.data() + envelope.RootConstantRecords.Offset,
        sizeof(root));
    EXPECT_EQ(root.StageMask, 1u << static_cast<uint8_t>(shader::ShaderStage::Vertex));
}

TEST(RadRayDxcMetadata, SupportedNonDefaultCompilePolicyCompiles) {
    constexpr std::string_view source = R"hlsl(
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}
)hlsl";

    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/non_default_policy.hlsl",
        CopyBytes(source),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded());

    shader::CompilePolicy policy{};
    policy.ShaderModel = 65;
    policy.Optimize = 0;
    policy.DebugInfo = 1;
    policy.AllResourcesBound = 1;
    policy.Warnings = shader::WarningPolicy::WarningsAsErrors;
    policy.HlslVersion = 2018;
    const auto result = client.CompileVariant(
        shader::CompileVariantRequest{
            .SourceName = "fixtures/non_default_policy.hlsl",
            .RootSource = CopyBytes(source),
            .Defines = {},
            .Assignments = {},
            .Targets = shader::ShaderTargetMask::All,
            .Policy = policy,
            .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    ASSERT_EQ(result.Status, shader::CompileStatus::Success)
        << (result.Diagnostics.empty() ? "" : result.Diagnostics.back().Message);
    EXPECT_EQ(result.Lanes.size(), 2u);

    for (const uint32_t hlslVersion : {2016u, 2017u, 2018u, 2021u}) {
        policy = {};
        policy.HlslVersion = hlslVersion;
        const auto versionResult = client.CompileVariant(
            shader::CompileVariantRequest{
                .SourceName = "fixtures/non_default_policy.hlsl",
                .RootSource = CopyBytes(source),
                .Defines = {},
                .Assignments = {},
                .Targets = shader::ShaderTargetMask::All,
                .Policy = policy,
                .ExpectedContract = discovery.Contract.Hash},
            includePaths);
        EXPECT_EQ(versionResult.Status, shader::CompileStatus::Success)
            << "HLSL " << hlslVersion << ": "
            << (versionResult.Diagnostics.empty()
                    ? ""
                    : versionResult.Diagnostics.back().Message);
        EXPECT_EQ(versionResult.Lanes.size(), 2u) << "HLSL " << hlslVersion;
    }
}

// Regression: discovery parses the whole translation unit with one library
// profile. While that profile was hardcoded to lib_6_1, any language feature
// gated on a later shader model was rejected during discovery even though the
// concrete stage compile would have accepted it. ResourceDescriptorHeap needs
// SM 6.6, so this source only parses when the requested model reaches DXC.
constexpr std::string_view kShaderModel66Source = R"hlsl(
struct Payload {
    uint Value;
};

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    RWStructuredBuffer<Payload> destination = ResourceDescriptorHeap[0];
    destination[tid.x].Value = tid.x;
}
)hlsl";

TEST(RadRayDxcMetadata, DiscoveryHonorsRequestedShaderModel) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();

    shader::CompilePolicy policy{};
    policy.ShaderModel = 66;
    shader::SourceContractRequest request{};
    request.SourceName = "fixtures/shader_model_66.hlsl";
    request.RootSource = CopyBytes(kShaderModel66Source);
    request.Targets = shader::ShaderTargetMask::DXIL;
    request.Policy = policy;
    const auto discovery = client.DiscoverSourceContract(request, includePaths);
    ASSERT_TRUE(discovery.Succeeded())
        << (discovery.Diagnostics.empty() ? "" : discovery.Diagnostics.back().Message);
    ASSERT_EQ(discovery.Contract.EntryPoints.size(), 1u);
    EXPECT_EQ(discovery.Contract.EntryPoints[0].Stage, shader::ShaderStage::Compute);

    const auto result = client.CompileVariant(
        shader::CompileVariantRequest{
            .SourceName = "fixtures/shader_model_66.hlsl",
            .RootSource = CopyBytes(kShaderModel66Source),
            .Defines = {},
            .Assignments = {},
            .Targets = shader::ShaderTargetMask::DXIL,
            .Policy = policy,
            .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    ASSERT_EQ(result.Status, shader::CompileStatus::Success)
        << (result.Diagnostics.empty() ? "" : result.Diagnostics.back().Message);
    EXPECT_EQ(result.Lanes.size(), 1u);

    // The same source must still fail closed below its required shader model,
    // proving the profile tracks the request instead of always widening.
    policy.ShaderModel = 60;
    request.Policy = policy;
    const auto tooOld = client.DiscoverSourceContract(request, includePaths);
    EXPECT_FALSE(tooOld.Succeeded());
    EXPECT_FALSE(tooOld.Diagnostics.empty());
}

// Regression: every supported shader model must produce a library profile DXC
// actually recognizes. lib_6_0 does not exist, and the highest released library
// profile is lib_6_9, so both ends of the policy range are boundary cases.
TEST(RadRayDxcMetadata, EverySupportedShaderModelDiscovers) {
    constexpr std::string_view source = R"hlsl(
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}
)hlsl";

    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    for (uint32_t shaderModel = 60; shaderModel <= 69; ++shaderModel) {
        shader::CompilePolicy policy{};
        policy.ShaderModel = shaderModel;
        shader::SourceContractRequest request{};
        request.SourceName = "fixtures/shader_model_sweep.hlsl";
        request.RootSource = CopyBytes(source);
        request.Targets = shader::ShaderTargetMask::All;
        request.Policy = policy;
        const auto discovery = client.DiscoverSourceContract(request, includePaths);
        EXPECT_TRUE(discovery.Succeeded())
            << "SM " << shaderModel << ": "
            << (discovery.Diagnostics.empty() ? "" : discovery.Diagnostics.back().Message);
    }
}

// Guard: the build deploys the DLL from SDKs/radray_dxc, so a locally built but
// unpublished compiler is silently overwritten on the next build. Asserting the
// toolchain identity of the loaded compiler turns "tested the wrong DLL" from a
// silent pass into a failure.
TEST(RadRayDxcMetadata, LoadedCompilerReportsExpectedToolchainIdentity) {
    constexpr uint64_t kExpectedToolchainIdentity = 0x0000000001090210ull;
    constexpr std::string_view source = R"hlsl(
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}
)hlsl";

    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/toolchain_identity.hlsl",
        CopyBytes(source),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded())
        << (discovery.Diagnostics.empty() ? "" : discovery.Diagnostics.back().Message);
    const auto result = client.CompileVariant(
        shader::CompileVariantRequest{
            .SourceName = "fixtures/toolchain_identity.hlsl",
            .RootSource = CopyBytes(source),
            .Defines = {},
            .Assignments = {},
            .Targets = shader::ShaderTargetMask::DXIL,
            .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    ASSERT_EQ(result.Status, shader::CompileStatus::Success)
        << (result.Diagnostics.empty() ? "" : result.Diagnostics.back().Message);
    ASSERT_EQ(result.Lanes.size(), 1u);
    shader::WireMetadataEnvelope envelope{};
    ASSERT_GE(result.Lanes[0].Metadata.size(), sizeof(envelope));
    std::memcpy(&envelope, result.Lanes[0].Metadata.data(), sizeof(envelope));
    EXPECT_EQ(envelope.ToolchainIdentity, kExpectedToolchainIdentity);
    EXPECT_EQ(envelope.SchemaVersion, shader::kShaderMetadataSchemaVersion);
}

TEST(RadRayDxcMetadata, UnsupportedCompilePolicyFailsClosed) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const auto includePaths = ShaderIncludePaths();
    const auto discovery = client.DiscoverSourceContract(
        "fixtures/policy_validation.hlsl",
        CopyBytes(kMultipleRootConstantsSource),
        shader::ShaderTarget::DXIL,
        includePaths);
    ASSERT_TRUE(discovery.Succeeded())
        << (discovery.Diagnostics.empty() ? "" : discovery.Diagnostics.back().Message);

    vector<shader::CompilePolicy> invalidPolicies;
    shader::CompilePolicy policy{};
    policy.ShaderModel = 59;
    invalidPolicies.push_back(policy);
    policy = {};
    policy.Optimize = 2;
    invalidPolicies.push_back(policy);
    policy = {};
    policy.DebugInfo = 2;
    invalidPolicies.push_back(policy);
    policy = {};
    policy.AllResourcesBound = 2;
    invalidPolicies.push_back(policy);
    policy = {};
    policy.Warnings = static_cast<shader::WarningPolicy>(2);
    invalidPolicies.push_back(policy);
    policy = {};
    policy.SpirvTargetEnv = static_cast<shader::SpirvTargetEnvironment>(1);
    invalidPolicies.push_back(policy);
    policy = {};
    policy.HlslVersion = 2020;
    invalidPolicies.push_back(policy);
    policy = {};
    policy.Reserved = 1;
    invalidPolicies.push_back(policy);

    for (size_t index = 0; index < invalidPolicies.size(); ++index) {
        const auto result = client.CompileVariant(
            shader::CompileVariantRequest{
                .SourceName = "fixtures/policy_validation.hlsl",
                .RootSource = CopyBytes(kMultipleRootConstantsSource),
                .Defines = {},
                .Assignments = {},
                .Targets = shader::ShaderTargetMask::DXIL,
                .Policy = invalidPolicies[index],
                .ExpectedContract = discovery.Contract.Hash},
            includePaths);
        EXPECT_EQ(result.Status, shader::CompileStatus::InvalidRequest)
            << "policy case " << index;
        EXPECT_TRUE(result.Lanes.empty()) << "policy case " << index;
        EXPECT_FALSE(result.Diagnostics.empty()) << "policy case " << index;
    }
}

TEST(RadRayDxcMetadata, FailedLaneDoesNotPublishPartialBatch) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());

    constexpr std::string_view invalidSource = R"hlsl(
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return;
}
)hlsl";
    shader::CompileVariantRequest request{
        .SourceName = "fixtures/invalid.hlsl",
        .RootSource = CopyBytes(invalidSource),
        .Defines = {},
        .Assignments = {},
        .Targets = shader::ShaderTargetMask::All};
    const shader::CompileVariantResult result = client.CompileVariant(request, ShaderIncludePaths());
    EXPECT_EQ(result.Status, shader::CompileStatus::InvalidRequest);
    EXPECT_TRUE(result.Lanes.empty());
    EXPECT_FALSE(result.Diagnostics.empty());
}

}  // namespace
}  // namespace radray::shader_compiler
