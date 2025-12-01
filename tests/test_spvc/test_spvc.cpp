#include <gtest/gtest.h>

#include <radray/render/dxc.h>
#include <radray/render/spvc.h>
#include <radray/render/utility.h>

const char* SHADER_CODE = R"(
struct PreObjectData
{
    float4x4 model;
    float4x4 mvp;
    float4x4 modelInv;
};

struct PreCameraData
{
    float4x4 view;
    float4x4 proj;
    float4x4 viewProj;
    float3 posW;
};

struct DirLight
{
    float4 lightDirW;
    float4 lightColor;
};

struct GlobalData
{
    DirLight dirLight;
    float time;
};

struct VS_INPUT
{
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float3 nor : NORMAL0;
    [[vk::location(2)]] float2 uv  : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float3 posW : POSITION;
    float3 norW : NORMAL0;
    float2 uv  : TEXCOORD0;
};

[[vk::push_constant]] ConstantBuffer<PreObjectData> _Obj : register(b0);
[[vk::binding(0)]] ConstantBuffer<PreCameraData> _Camera : register(b1);
[[vk::binding(1)]] ConstantBuffer<GlobalData> _Global : register(b2);
[[vk::binding(2)]] cbuffer TestCBuffer : register(b3)
{
    float4 _SomeValue;
};

[[vk::binding(3)]] Texture2D _AlbedoTex : register(t0);
[[vk::binding(5)]] SamplerState _AlbedoSampler : register(s0);
[[vk::binding(4)]] Texture2D _MetallicRoughnessTex : register(t1);
[[vk::binding(6)]] SamplerState _MetallicRoughnessSampler : register(s1);

PS_INPUT VSMain(VS_INPUT vsIn)
{
    PS_INPUT psIn;
    psIn.pos = mul(float4(vsIn.pos + _Camera.posW, 1.0f), _Obj.mvp);
    psIn.posW = mul(float4(vsIn.pos + _Global.time, 1.0f), _Obj.model).xyz;
    psIn.norW = normalize(mul(float4(vsIn.nor, 0.0f), _Obj.modelInv).xyz);
    psIn.uv = vsIn.uv;
    return psIn;
}

float4 PSMain(PS_INPUT psIn) : SV_Target
{
    float3 albedo = _AlbedoTex.Sample(_AlbedoSampler, psIn.uv).xyz;
    float3 mr = _MetallicRoughnessTex.Sample(_MetallicRoughnessSampler, psIn.uv).xyz;
    float3 t = albedo * mr * ((float3)1.0 / _Global.dirLight.lightDirW.xyz) + ((float3)1.0 / + _Camera.posW);
    return float4(t, 1.0f) + _SomeValue;
}
)";

using namespace radray;
using namespace radray::render;

TEST(SPVC, BasicReflection) {
    auto dxc = CreateDxc();
    auto vs = dxc->Compile(SHADER_CODE, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, true, {}, {}, true).value();
    auto ps = dxc->Compile(SHADER_CODE, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, true, {}, {}, true).value();

    auto vsDesc = ReflectSpirv("VSMain", ShaderStage::Vertex, vs.Data);
    ASSERT_TRUE(vsDesc.has_value());
    auto psDesc = ReflectSpirv("PSMain", ShaderStage::Pixel, ps.Data);
    ASSERT_TRUE(psDesc.has_value());
}
