#include <gtest/gtest.h>
#include <iostream>

#include <radray/render/dxc.h>
#include <radray/render/spvc.h>

using namespace radray;
using namespace radray::render;

// 简单的 VS/PS shader，带 push constant、cbuffer、texture、sampler
const char* HLSL_VS_PS = R"(
struct VS_INPUT
{
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float2 uv  : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

struct PreObject
{
    float4x4 mvp;
};

[[vk::push_constant]] ConstantBuffer<PreObject> _Obj : register(b0);
[[vk::binding(0)]] Texture2D _Tex : register(t0);
[[vk::binding(1)]] SamplerState _Sampler : register(s0);

PS_INPUT VSMain(VS_INPUT vsIn)
{
    PS_INPUT o;
    o.pos = mul(float4(vsIn.pos, 1.0f), _Obj.mvp);
    o.uv = vsIn.uv;
    return o;
}

float4 PSMain(PS_INPUT psIn) : SV_Target
{
    return _Tex.Sample(_Sampler, psIn.uv);
}
)";

const char* HLSL_COMPUTE = R"(
RWStructuredBuffer<float4> _Output : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    _Output[dtid.x] = float4(dtid.x, dtid.y, dtid.z, 1.0f);
}
)";

TEST(SpirvToMsl, VertexShader) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto vs = dxc->Compile(HLSL_VS_PS, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(vs.has_value());

    auto result = ConvertSpirvToMsl(vs->Data, "VSMain", ShaderStage::Vertex);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->MslSource.empty());
    EXPECT_FALSE(result->EntryPointName.empty());
    EXPECT_NE(result->MslSource.find("vertex"), std::string::npos);
    std::cout << "=== Vertex MSL (entry: " << result->EntryPointName << ") ===\n"
              << result->MslSource << "\n";
}

TEST(SpirvToMsl, PixelShader) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto ps = dxc->Compile(HLSL_VS_PS, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(ps.has_value());

    auto result = ConvertSpirvToMsl(ps->Data, "PSMain", ShaderStage::Pixel);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->MslSource.empty());
    EXPECT_FALSE(result->EntryPointName.empty());
    EXPECT_NE(result->MslSource.find("fragment"), std::string::npos);
    std::cout << "=== Fragment MSL (entry: " << result->EntryPointName << ") ===\n"
              << result->MslSource << "\n";
}

TEST(SpirvToMsl, ComputeShader) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto cs = dxc->Compile(HLSL_COMPUTE, "CSMain", ShaderStage::Compute, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(cs.has_value());

    auto result = ConvertSpirvToMsl(cs->Data, "CSMain", ShaderStage::Compute);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->MslSource.empty());
    EXPECT_FALSE(result->EntryPointName.empty());
    EXPECT_NE(result->MslSource.find("kernel"), std::string::npos);
    std::cout << "=== Compute MSL (entry: " << result->EntryPointName << ") ===\n"
              << result->MslSource << "\n";
}

TEST(SpirvToMsl, MslVersionOption) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto vs = dxc->Compile(HLSL_VS_PS, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(vs.has_value());

    SpirvToMslOption opt{};
    opt.MslMajor = 3;
    opt.MslMinor = 0;
    auto result = ConvertSpirvToMsl(vs->Data, "VSMain", ShaderStage::Vertex, opt);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->MslSource.empty());
}

TEST(SpirvToMsl, InvalidSpirvData) {
    std::vector<byte> garbage(16, byte{0xFF});
    auto result = ConvertSpirvToMsl(garbage, "main", ShaderStage::Vertex);
    EXPECT_FALSE(result.has_value());
}

TEST(SpirvToMsl, InvalidSpirvSize) {
    // 非4字节对齐
    std::vector<byte> bad(13, byte{0x00});
    auto result = ConvertSpirvToMsl(bad, "main", ShaderStage::Vertex);
    EXPECT_FALSE(result.has_value());
}
