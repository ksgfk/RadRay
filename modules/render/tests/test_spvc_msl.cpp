#include <algorithm>
#include <optional>
#include <string_view>

#include <gtest/gtest.h>

#include <radray/render/shader_compiler/dxc.h>
#include <radray/render/shader_compiler/spvc.h>

namespace radray::render {
namespace {

constexpr std::string_view kVertexShader = R"(
struct VsInput {
    float3 Position : POSITION;
};

struct VsOutput {
    float4 Position : SV_Position;
};

struct PushData {
    float4 Offset;
};

struct SceneData {
    float4 Scale;
};

[[vk::push_constant]] ConstantBuffer<PushData> gPush : register(b0, space0);
[[vk::binding(2, 0)]] ConstantBuffer<SceneData> gScene : register(b2, space0);

VsOutput VSMain(VsInput input) {
    VsOutput output;
    output.Position = float4(input.Position, 1.0) * gScene.Scale + gPush.Offset;
    return output;
}
)";

constexpr std::string_view kPixelShaderWithArgumentBuffer = R"(
struct PsInput {
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

struct MaterialData {
    float4 Tint;
};

[[vk::binding(0, 1)]] ConstantBuffer<MaterialData> gMaterial : register(b0, space1);
[[vk::binding(1, 1)]] Texture2D<float4> gTex : register(t1, space1);
[[vk::binding(2, 1)]] SamplerState gSamp : register(s2, space1);

float4 PSMain(PsInput input) : SV_Target0 {
    return gTex.Sample(gSamp, input.UV) * gMaterial.Tint;
}
)";

constexpr std::string_view kStructuredBufferShader = R"(
struct Light {
    float4 Direction;
    float4 Irradiance;
};

[[vk::binding(0, 0)]] StructuredBuffer<Light> gReadOnlyLights : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<Light> gReadWriteLights : register(u1, space0);

[numthreads(1, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    gReadWriteLights[tid.x] = gReadOnlyLights[tid.x];
}
)";

std::optional<DxcOutput> CompileSpirv(
    Dxc& dxc,
    std::string_view source,
    std::string_view entryPoint,
    ShaderStage stage) {
    DxcCompileOptions options{};
    options.EntryPoint = entryPoint;
    options.Stage = stage;
    options.SM = HlslShaderModel::SM60;
    options.IsOptimize = false;
    options.IsSpirv = true;
    options.EnableUnbounded = false;
    return dxc.CompileMemory(source, "test_spvc_msl.hlsl", options);
}

const MslArgument* FindArgumentByName(const MslShaderReflection& reflection, std::string_view name) {
    auto it = std::find_if(reflection.Arguments.begin(), reflection.Arguments.end(), [&](const MslArgument& argument) {
        return argument.Name == name;
    });
    return it == reflection.Arguments.end() ? nullptr : &(*it);
}

TEST(SpvcMslTest, AppliesExplicitVertexBufferIndicesToSourceAndReflection) {
    auto dxcOpt = CreateDxc();
    ASSERT_TRUE(dxcOpt.HasValue());
    shared_ptr<Dxc> dxc = dxcOpt.Release();

    auto spirvOpt = CompileSpirv(*dxc, kVertexShader, "VSMain", ShaderStage::Vertex);
    ASSERT_TRUE(spirvOpt.has_value());

    SpirvToMslOption option{};
    option.VertexStageBufferStartIndex = 7;
    option.PushConstantBufferIndex = 3;

    auto mslOpt = ConvertSpirvToMsl(spirvOpt->Data, "VSMain", ShaderStage::Vertex, option);
    ASSERT_TRUE(mslOpt.has_value());
    EXPECT_NE(mslOpt->MslSource.find("[[buffer(9)]]"), string::npos);
    EXPECT_NE(mslOpt->MslSource.find("[[buffer(3)]]"), string::npos);

    SpirvAsMslReflectParams reflectParams{
        .SpirV = spirvOpt->Data,
        .EntryPoint = "VSMain",
        .Stage = ShaderStage::Vertex,
        .UseArgumentBuffers = false,
        .VertexStageBufferStartIndex = option.VertexStageBufferStartIndex,
        .FragmentStageBufferStartIndex = option.FragmentStageBufferStartIndex,
        .PushConstantBufferIndex = option.PushConstantBufferIndex,
    };
    auto reflectionOpt = ReflectSpirvAsMsl(std::span<const SpirvAsMslReflectParams>{&reflectParams, 1});
    ASSERT_TRUE(reflectionOpt.has_value());

    const MslArgument* scene = FindArgumentByName(*reflectionOpt, "gScene");
    ASSERT_NE(scene, nullptr);
    EXPECT_EQ(scene->Index, 9u);
    EXPECT_EQ(scene->Type, MslArgumentType::Buffer);

    const MslArgument* push = FindArgumentByName(*reflectionOpt, "gPush");
    ASSERT_NE(push, nullptr);
    EXPECT_TRUE(push->IsPushConstant);
    EXPECT_EQ(push->Index, 3u);
}

TEST(SpvcMslTest, AppliesExplicitArgumentBufferStartIndexToFragmentSource) {
    auto dxcOpt = CreateDxc();
    ASSERT_TRUE(dxcOpt.HasValue());
    shared_ptr<Dxc> dxc = dxcOpt.Release();

    auto spirvOpt = CompileSpirv(*dxc, kPixelShaderWithArgumentBuffer, "PSMain", ShaderStage::Pixel);
    ASSERT_TRUE(spirvOpt.has_value());

    SpirvToMslOption option{};
    option.UseArgumentBuffers = true;
    option.FragmentStageBufferStartIndex = 11;

    auto mslOpt = ConvertSpirvToMsl(spirvOpt->Data, "PSMain", ShaderStage::Pixel, option);
    ASSERT_TRUE(mslOpt.has_value());
    EXPECT_NE(mslOpt->MslSource.find("[[buffer(12)]]"), string::npos);
}

// Regression: HLSL StructuredBuffer (read-only) and RWStructuredBuffer both lower to a SPIR-V
// StorageBuffer. DXC marks the read-only one with a NonWritable decoration on the *block member*,
// not on the variable. ReflectSpirv must inspect get_buffer_block_flags (which merges member
// decorations) so a read-only StructuredBuffer maps to ResourceBindType::Buffer instead of RWBuffer.
TEST(SpvcMslTest, ReflectsReadOnlyStructuredBufferAsReadOnly) {
    auto dxcOpt = CreateDxc();
    ASSERT_TRUE(dxcOpt.HasValue());
    shared_ptr<Dxc> dxc = dxcOpt.Release();

    auto spirvOpt = CompileSpirv(*dxc, kStructuredBufferShader, "CSMain", ShaderStage::Compute);
    ASSERT_TRUE(spirvOpt.has_value());

    auto reflectionOpt = ReflectSpirv(SpirvBytecodeView{
        .Data = spirvOpt->Data,
        .EntryPointName = "CSMain",
        .Stage = ShaderStage::Compute,
    });
    ASSERT_TRUE(reflectionOpt.has_value());

    auto findBinding = [&](std::string_view name) -> const SpirvResourceBinding* {
        auto it = std::find_if(
            reflectionOpt->ResourceBindings.begin(),
            reflectionOpt->ResourceBindings.end(),
            [&](const SpirvResourceBinding& b) { return b.Name == name; });
        return it == reflectionOpt->ResourceBindings.end() ? nullptr : &(*it);
    };

    const SpirvResourceBinding* readOnly = findBinding("gReadOnlyLights");
    ASSERT_NE(readOnly, nullptr);
    EXPECT_EQ(readOnly->Kind, SpirvResourceKind::StorageBuffer);
    EXPECT_TRUE(readOnly->ReadOnly);
    EXPECT_FALSE(readOnly->WriteOnly);

    const SpirvResourceBinding* readWrite = findBinding("gReadWriteLights");
    ASSERT_NE(readWrite, nullptr);
    EXPECT_EQ(readWrite->Kind, SpirvResourceKind::StorageBuffer);
    EXPECT_FALSE(readWrite->ReadOnly);
    EXPECT_FALSE(readWrite->WriteOnly);
}

}  // namespace
}  // namespace radray::render
