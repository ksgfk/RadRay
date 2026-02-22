#include <gtest/gtest.h>
#include <iostream>

#include <radray/render/dxc.h>
#include <radray/render/msl.h>

using namespace radray;
using namespace radray::render;

// 简单 compute shader，带 RWStructuredBuffer
const char* HLSL_COMPUTE = R"(
RWStructuredBuffer<float4> _Output : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    _Output[dtid.x] = float4(dtid.x, dtid.y, dtid.z, 1.0f);
}
)";

// compute shader，带 cbuffer 结构体
const char* HLSL_COMPUTE_CBUFFER = R"(
cbuffer Params : register(b0)
{
    float4 color;
    float intensity;
};

RWStructuredBuffer<float4> _Output : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    _Output[dtid.x] = color * intensity;
}
)";

// VS/PS shader，带 texture、sampler、push constant
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

static std::optional<DxcOutput> CompileSpirv(
    Dxc& dxc,
    const char* hlsl,
    const char* entry,
    ShaderStage stage) {
    return dxc.Compile(hlsl, entry, stage, HlslShaderModel::SM60, true, {}, {}, true);
}

TEST(HlslToMslRefl, ComputeShader) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto cs = CompileSpirv(*dxc, HLSL_COMPUTE, "CSMain", ShaderStage::Compute);
    ASSERT_TRUE(cs.has_value());

    MslReflectParams params{cs->Data, "CSMain", ShaderStage::Compute};
    auto refl = ReflectMsl(std::span{&params, 1});
    ASSERT_TRUE(refl.has_value());
    EXPECT_FALSE(refl->Arguments.empty());

    // 应该有一个 RWStructuredBuffer → Buffer 类型
    bool foundOutput = false;
    for (const auto& arg : refl->Arguments) {
        std::cout << "  arg: " << arg.Name << " type=" << format_as(arg.Type)
                  << " stage=" << format_as(arg.Stage)
                  << " index=" << arg.Index << "\n";
        if (arg.Name == "_Output") {
            foundOutput = true;
            EXPECT_EQ(arg.Type, MslArgumentType::Buffer);
            EXPECT_EQ(arg.Stage, MslStage::Compute);
            // compute shader 不偏移，buffer index 应该是 0
            EXPECT_EQ(arg.Index, 0u);
            // RWStructuredBuffer 应该是 ReadWrite
            EXPECT_EQ(arg.Access, MslAccess::ReadWrite);
        }
    }
    EXPECT_TRUE(foundOutput) << "_Output not found in reflection";
}

TEST(HlslToMslRefl, ComputeCBuffer) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto cs = CompileSpirv(*dxc, HLSL_COMPUTE_CBUFFER, "CSMain", ShaderStage::Compute);
    ASSERT_TRUE(cs.has_value());

    MslReflectParams params{cs->Data, "CSMain", ShaderStage::Compute};
    auto refl = ReflectMsl(std::span{&params, 1});
    ASSERT_TRUE(refl.has_value());

    // 应该有 cbuffer (Params) 和 RWStructuredBuffer (_Output)
    bool foundParams = false;
    for (const auto& arg : refl->Arguments) {
        std::cout << "  arg: " << arg.Name << " type=" << format_as(arg.Type)
                  << " bufferDataType=" << format_as(arg.BufferDataType)
                  << " index=" << arg.Index << "\n";
        if (arg.Type == MslArgumentType::Buffer && arg.Name == "Params") {
            foundParams = true;
            EXPECT_EQ(arg.BufferDataType, MslDataType::Struct);
            ASSERT_NE(arg.BufferStructTypeIndex, UINT32_MAX);
            ASSERT_LT(arg.BufferStructTypeIndex, refl->StructTypes.size());
            const auto& st = refl->StructTypes[arg.BufferStructTypeIndex];
            // Params 结构体应该有 color (float4) 和 intensity (float)
            EXPECT_GE(st.Members.size(), 2u);
            for (const auto& m : st.Members) {
                std::cout << "    member: " << m.Name
                          << " offset=" << m.Offset
                          << " type=" << format_as(m.DataType) << "\n";
            }
        }
    }
    EXPECT_TRUE(foundParams) << "cbuffer struct not found in reflection";
}

TEST(HlslToMslRefl, RenderPipeline) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto vs = CompileSpirv(*dxc, HLSL_VS_PS, "VSMain", ShaderStage::Vertex);
    ASSERT_TRUE(vs.has_value());
    auto ps = CompileSpirv(*dxc, HLSL_VS_PS, "PSMain", ShaderStage::Pixel);
    ASSERT_TRUE(ps.has_value());

    MslReflectParams params[] = {
        {vs->Data, "VSMain", ShaderStage::Vertex},
        {ps->Data, "PSMain", ShaderStage::Pixel},
    };
    auto refl = ReflectMsl(params);
    ASSERT_TRUE(refl.has_value());

    bool hasVertex = false;
    bool hasFragment = false;
    bool foundTexture = false;
    bool foundSampler = false;
    for (const auto& arg : refl->Arguments) {
        std::cout << "  arg: " << arg.Name << " type=" << format_as(arg.Type)
                  << " stage=" << format_as(arg.Stage)
                  << " index=" << arg.Index << "\n";
        if (arg.Stage == MslStage::Vertex) hasVertex = true;
        if (arg.Stage == MslStage::Fragment) hasFragment = true;
        if (arg.Type == MslArgumentType::Texture) foundTexture = true;
        if (arg.Type == MslArgumentType::Sampler) foundSampler = true;
    }
    EXPECT_TRUE(hasVertex) << "no vertex stage arguments found";
    EXPECT_TRUE(hasFragment) << "no fragment stage arguments found";
    EXPECT_TRUE(foundTexture) << "no texture argument found";
    EXPECT_TRUE(foundSampler) << "no sampler argument found";
}

TEST(HlslToMslRefl, VertexBufferOffset) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    auto vs = CompileSpirv(*dxc, HLSL_VS_PS, "VSMain", ShaderStage::Vertex);
    ASSERT_TRUE(vs.has_value());

    MslReflectParams params{vs->Data, "VSMain", ShaderStage::Vertex};
    auto refl = ReflectMsl(std::span{&params, 1});
    ASSERT_TRUE(refl.has_value());

    // vertex stage 的 buffer 应该有 +16 偏移
    for (const auto& arg : refl->Arguments) {
        if (arg.Type == MslArgumentType::Buffer) {
            std::cout << "  buffer: " << arg.Name << " index=" << arg.Index << "\n";
            EXPECT_GE(arg.Index, MetalMaxVertexInputBindings)
                << "vertex buffer " << arg.Name << " should have index >= 16";
        }
    }
}

TEST(HlslToMslRefl, InvalidSpirv) {
    std::vector<byte> bad(13, byte{0x00});
    MslReflectParams params{bad, "main", ShaderStage::Compute};
    auto refl = ReflectMsl(std::span{&params, 1});
    EXPECT_FALSE(refl.has_value());
}
