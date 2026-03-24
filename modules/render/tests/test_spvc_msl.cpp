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

std::optional<DxcOutput> CompileSpirv(
    Dxc& dxc,
    std::string_view source,
    std::string_view entryPoint,
    ShaderStage stage) {
    DxcCompileParams params{};
    params.Code = source;
    params.EntryPoint = entryPoint;
    params.Stage = stage;
    params.SM = HlslShaderModel::SM60;
    params.IsOptimize = false;
    params.IsSpirv = true;
    params.EnableUnbounded = false;
    return dxc.Compile(params);
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

}  // namespace
}  // namespace radray::render
