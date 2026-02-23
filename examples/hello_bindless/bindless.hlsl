#include <common.hlsl>

struct PreObjectData
{
    float4x4 mvp;
    uint baseTexIndex;
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
VK_BINDING(0, 0) Texture2D<float4> _Tex[] : register(t0);
VK_BINDING(0, 1) SamplerState _Sampler : register(s0);

PS_INPUT VSMain(VS_INPUT vsIn)
{
    PS_INPUT psIn;
    psIn.pos = mul(_Obj.mvp, float4(vsIn.pos, 1.0));
    psIn.uv = vsIn.uv;
    return psIn;
}

float4 PSMain(PS_INPUT psIn) : SV_Target
{
    // 根据UV判断象限: 左上0 右上1 左下2 右下3
    uint qx = psIn.uv.x >= 0.5 ? 1 : 0;
    uint qy = psIn.uv.y >= 0.5 ? 1 : 0;
    uint quadrant = qy * 2 + qx;
    // 将UV重映射到[0,1]
    float2 localUV = frac(psIn.uv * 2.0);
    float4 color = _Tex[NonUniformResourceIndex(_Obj.baseTexIndex + quadrant)].Sample(_Sampler, localUV);
    return float4(color.rgb, 1.0);
}
