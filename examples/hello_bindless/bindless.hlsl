#include <common.hlsl>

struct PreObjectData
{
    float4x4 mvp;
    uint texIndex;
    uint sIndex;
};

struct VS_INPUT
{
    VK_LOCATION(0) float3 pos : POSITION;
    VK_LOCATION(1) float2 uv  : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    VK_LOCATION(0) float2 uv  : TEXCOORD0;
};

VK_PUSH_CONSTANT ConstantBuffer<PreObjectData> _Obj : register(b0);
Texture2D<float4> _Tex[] : register(t0);
SamplerState _Sampler[] : register(s0);

PS_INPUT VSMain(VS_INPUT vsIn)
{
    PS_INPUT psIn;
    psIn.pos = mul(_Obj.mvp, float4(vsIn.pos, 1.0));
    psIn.uv = vsIn.uv;
    return psIn;
}

float4 PSMain(PS_INPUT psIn) : SV_Target
{
    float4 color = _Tex[_Obj.texIndex].Sample(_Sampler[_Obj.sIndex], psIn.uv);
    return float4(color.rgb, 1.0);
}
