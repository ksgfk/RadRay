struct PreObjectData
{
    float4x4 model;
    float4x4 mvp;
};

struct PreCameraData
{
    float4x4 view;
    float4x4 proj;
    float4x4 viewProj;
    float3 posW;
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
    [[vk::location(0)]] float3 nor : NORMAL0;
    [[vk::location(1)]] float2 uv  : TEXCOORD0;
};

[[vk::push_constant]] ConstantBuffer<PreObjectData> _Obj : register(b0);
[[vk::binding(1)]] ConstantBuffer<PreCameraData> _Camera : register(b1);

PS_INPUT VSMain(VS_INPUT vsIn)
{
    PS_INPUT psIn;
    psIn.pos = mul(_Obj.mvp, float4(vsIn.pos, 1.0));
    psIn.nor = normalize(mul((float3x3)_Camera.view, vsIn.nor));
    psIn.uv = vsIn.uv;
    return psIn;
}

float4 PSMain(PS_INPUT psIn) : SV_Target
{
    return float4(psIn.nor * 0.5 + 0.5, 1.0);
}
