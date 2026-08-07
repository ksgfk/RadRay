#include <radray/shader_compiler/client.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
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

Texture2D<float4> AlbedoTexture : register(t0, space1);
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
[RootSignature("StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT)")]
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return float4(1.0, 1.0, 1.0, 1.0);
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
[RootSignature("CBV(b0)")]
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[RootSignature("CBV(b1)")]
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

constexpr std::string_view kStaticSamplerSuccessSource = R"hlsl(
#include <core/platform.hlsli>

VK_BINDING(1, 4)
Texture2D<float4> ColorTexture;
VK_BINDING(2, 4)
SamplerState ColorSampler;

#if !defined(__spirv__)
[RootSignature("StaticSampler(s0, filter=FILTER_MIN_MAG_LINEAR_MIP_POINT)")]
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

TEST(RadRayDxcMetadata, ExplicitRootSignatureMustMatchActiveResourceUnion) {
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
    EXPECT_EQ(result.Status, shader::CompileStatus::TargetFailure);
    EXPECT_TRUE(result.Lanes.empty());
    const bool hasRootDiagnostic = std::any_of(
        result.Diagnostics.begin(),
        result.Diagnostics.end(),
        [](const shader::CompileDiagnostic& diagnostic) noexcept {
            return diagnostic.Code == 2106;
        });
    if (!hasRootDiagnostic) {
        for (const shader::CompileDiagnostic& diagnostic : result.Diagnostics) {
            ADD_FAILURE() << diagnostic.Code << ": " << diagnostic.Message;
        }
    }
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
                EXPECT_NE(binding.Flags & 1u, 0u);
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
    const auto result = client.CompileVariant(shader::CompileVariantRequest{
        .SourceName = "fixtures/mismatched_graphics_root_signatures.hlsl",
        .RootSource = CopyBytes(kMismatchedGraphicsRootSignaturesSource),
        .Defines = {},
        .Assignments = {},
        .Targets = shader::ShaderTargetMask::DXIL,
        .ExpectedContract = discovery.Contract.Hash},
        includePaths);
    EXPECT_EQ(result.Status, shader::CompileStatus::TargetFailure);
    EXPECT_TRUE(result.Lanes.empty());
    EXPECT_FALSE(result.Diagnostics.empty());
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
