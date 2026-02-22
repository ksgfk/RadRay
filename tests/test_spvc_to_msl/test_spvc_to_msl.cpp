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

// 带 cbuffer 的 shader，用于测试 buffer 偏移
const char* HLSL_WITH_CBUFFER = R"(
struct VS_INPUT
{
    [[vk::location(0)]] float3 pos : POSITION;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
};

cbuffer SceneData : register(b0, space0)
{
    float4x4 viewProj;
};

PS_INPUT VSMain(VS_INPUT vsIn)
{
    PS_INPUT o;
    o.pos = mul(float4(vsIn.pos, 1.0f), viewProj);
    return o;
}
)";

TEST(SpirvToMsl, VertexBufferOffset) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto vs = dxc->Compile(HLSL_VS_PS, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(vs.has_value());

    auto result = ConvertSpirvToMsl(vs->Data, "VSMain", ShaderStage::Vertex);
    ASSERT_TRUE(result.has_value());
    auto& msl = result->MslSource;
    std::cout << "=== VertexBufferOffset MSL ===\n" << msl << "\n";

    // push constant 应该在 buffer(16)，即 MetalMaxVertexInputBindings
    EXPECT_NE(msl.find("buffer(16)"), std::string::npos)
        << "push constant should be at buffer(16)";
    // 不应该有 buffer(0)（push constant 不应该在 0）
    EXPECT_EQ(msl.find("buffer(0)"), std::string::npos)
        << "no buffer resource should be at buffer(0), reserved for vertex buffers";
}

TEST(SpirvToMsl, FragmentBufferOffset) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto ps = dxc->Compile(HLSL_VS_PS, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(ps.has_value());

    auto result = ConvertSpirvToMsl(ps->Data, "PSMain", ShaderStage::Pixel);
    ASSERT_TRUE(result.has_value());
    auto& msl = result->MslSource;
    std::cout << "=== FragmentBufferOffset MSL ===\n" << msl << "\n";

    // texture 应该保持在 texture(0)，不偏移
    EXPECT_NE(msl.find("texture(0)"), std::string::npos)
        << "texture should stay at texture(0)";
    // sampler 应该保持不偏移
    EXPECT_NE(msl.find("sampler(0)"), std::string::npos)
        << "sampler should stay at sampler(0)";
}

TEST(SpirvToMsl, ComputeNoOffset) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto cs = dxc->Compile(HLSL_COMPUTE, "CSMain", ShaderStage::Compute, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(cs.has_value());

    auto result = ConvertSpirvToMsl(cs->Data, "CSMain", ShaderStage::Compute);
    ASSERT_TRUE(result.has_value());
    auto& msl = result->MslSource;
    std::cout << "=== ComputeNoOffset MSL ===\n" << msl << "\n";

    // compute shader 不需要偏移，buffer 应该在 buffer(0)
    EXPECT_NE(msl.find("buffer(0)"), std::string::npos)
        << "compute buffer should stay at buffer(0)";
    // 不应该有 buffer(16)
    EXPECT_EQ(msl.find("buffer(16)"), std::string::npos)
        << "compute should not have buffer offset";
}

// 多 descriptor set 的 shader，用于测试 argument buffer 多 set 偏移
const char* HLSL_MULTI_SPACE = R"(
struct VS_INPUT
{
    [[vk::location(0)]] float3 pos : POSITION;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
};

cbuffer SceneData : register(b0, space0)
{
    float4x4 viewProj;
};

cbuffer ObjectData : register(b0, space1)
{
    float4x4 world;
};

PS_INPUT VSMain(VS_INPUT vsIn)
{
    PS_INPUT o;
    o.pos = mul(mul(float4(vsIn.pos, 1.0f), world), viewProj);
    return o;
}
)";

// VS shader with both push constant and descriptor set resources
const char* HLSL_VS_WITH_PUSH_AND_DESC = R"(
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

struct PushData
{
    float4x4 mvp;
};

[[vk::push_constant]] ConstantBuffer<PushData> _Push : register(b0);

cbuffer SceneData : register(b0, space0)
{
    float4 lightDir;
};

PS_INPUT VSMain(VS_INPUT vsIn)
{
    PS_INPUT o;
    o.pos = mul(float4(vsIn.pos + lightDir.xyz, 1.0f), _Push.mvp);
    o.uv = vsIn.uv;
    return o;
}
)";

// --- Argument Buffer Tests ---

TEST(SpirvToMsl, ArgBufferVsPushConstantNoOverlap) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto vs = dxc->Compile(HLSL_VS_WITH_PUSH_AND_DESC, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(vs.has_value());

    SpirvToMslOption opt{};
    opt.MslMajor = 3;
    opt.MslMinor = 0;
    opt.UseArgumentBuffers = true;
    auto result = ConvertSpirvToMsl(vs->Data, "VSMain", ShaderStage::Vertex, opt);
    ASSERT_TRUE(result.has_value());
    auto& msl = result->MslSource;
    std::cout << "=== ArgBufferVsPushConstantNoOverlap MSL ===\n" << msl << "\n";

    // push constant should be at buffer(16)
    EXPECT_NE(msl.find("buffer(16)"), std::string::npos)
        << "push constant should be at buffer(16)";
    // argument buffer for desc set 0 should be at buffer(17), not buffer(16)
    EXPECT_NE(msl.find("buffer(17)"), std::string::npos)
        << "argument buffer for desc set 0 should be at buffer(17)";
}

TEST(SpirvToMsl, ArgBufferPsPushConstantNoOverlap) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto ps = dxc->Compile(HLSL_VS_PS, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(ps.has_value());

    SpirvToMslOption opt{};
    opt.MslMajor = 3;
    opt.MslMinor = 0;
    opt.UseArgumentBuffers = true;
    auto result = ConvertSpirvToMsl(ps->Data, "PSMain", ShaderStage::Pixel, opt);
    ASSERT_TRUE(result.has_value());
    auto& msl = result->MslSource;
    std::cout << "=== ArgBufferPsPushConstantNoOverlap MSL ===\n" << msl << "\n";

    // argument buffer for desc set 0 should be at buffer(17)
    EXPECT_NE(msl.find("buffer(17)"), std::string::npos)
        << "argument buffer for desc set 0 should be at buffer(17)";
    // buffer(16) should not appear (PS has no push constant in HLSL_VS_PS)
    // or if it does, it must not collide with argument buffer
}

TEST(SpirvToMsl, ArgBufferMultiDescriptorSet) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto vs = dxc->Compile(HLSL_MULTI_SPACE, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(vs.has_value());

    SpirvToMslOption opt{};
    opt.MslMajor = 3;
    opt.MslMinor = 0;
    opt.UseArgumentBuffers = true;
    auto result = ConvertSpirvToMsl(vs->Data, "VSMain", ShaderStage::Vertex, opt);
    ASSERT_TRUE(result.has_value());
    auto& msl = result->MslSource;
    std::cout << "=== ArgBufferMultiDescriptorSet MSL ===\n" << msl << "\n";

    // desc set 0 -> buffer(17), desc set 1 -> buffer(18)
    EXPECT_NE(msl.find("buffer(17)"), std::string::npos)
        << "argument buffer for desc set 0 should be at buffer(17)";
    EXPECT_NE(msl.find("buffer(18)"), std::string::npos)
        << "argument buffer for desc set 1 should be at buffer(18)";
    // no argument buffer should be at buffer(0)-buffer(15) (vertex buffer range)
    for (int i = 0; i <= 15; i++) {
        std::string pat = "buffer(" + std::to_string(i) + ")";
        EXPECT_EQ(msl.find(pat), std::string::npos)
            << "no argument buffer should be at " << pat;
    }
}

TEST(SpirvToMsl, ArgBufferComputeNoOffset) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto cs = dxc->Compile(HLSL_COMPUTE, "CSMain", ShaderStage::Compute, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(cs.has_value());

    SpirvToMslOption opt{};
    opt.MslMajor = 3;
    opt.MslMinor = 0;
    opt.UseArgumentBuffers = true;
    auto result = ConvertSpirvToMsl(cs->Data, "CSMain", ShaderStage::Compute, opt);
    ASSERT_TRUE(result.has_value());
    auto& msl = result->MslSource;
    std::cout << "=== ArgBufferComputeNoOffset MSL ===\n" << msl << "\n";

    // compute shader should not have buffer(17+) offset
    EXPECT_EQ(msl.find("buffer(17)"), std::string::npos)
        << "compute should not have argument buffer offset";
    EXPECT_EQ(msl.find("buffer(16)"), std::string::npos)
        << "compute should not have push constant offset";
}

TEST(SpirvToMsl, CBufferOffset) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto vs = dxc->Compile(HLSL_WITH_CBUFFER, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true, {}, {}, true);
    ASSERT_TRUE(vs.has_value());

    auto result = ConvertSpirvToMsl(vs->Data, "VSMain", ShaderStage::Vertex);
    ASSERT_TRUE(result.has_value());
    auto& msl = result->MslSource;
    std::cout << "=== CBufferOffset MSL ===\n" << msl << "\n";

    // cbuffer 应该在 buffer(16)
    EXPECT_NE(msl.find("buffer(16)"), std::string::npos)
        << "cbuffer should be at buffer(16)";
}
