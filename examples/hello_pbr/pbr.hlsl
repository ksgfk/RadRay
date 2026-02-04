#include <common.hlsli>
#include <bsdf.hlsli>
#include <light.hlsli>

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

struct VS_INPUT
{
    VK_LOCATION(0) float3 pos : POSITION;
    VK_LOCATION(1) float3 nor : NORMAL0;
    VK_LOCATION(2) float2 uv  : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    VK_LOCATION(0) float3 posW : POSITION;
    VK_LOCATION(1) float3 norW : NORMAL0;
    VK_LOCATION(2) float2 uv   : TEXCOORD0;
};

VK_PUSH_CONSTANT ConstantBuffer<PreObjectData> _Obj : register(b0);
VK_BINDING(0, 0) ConstantBuffer<PreCameraData> _Camera : register(b1);

PS_INPUT VSMain(VS_INPUT vsIn)
{
    PS_INPUT psIn;
    psIn.pos = mul(_Obj.mvp, float4(vsIn.pos, 1.0));
    float4 posW = mul(_Obj.model, float4(vsIn.pos, 1.0));
    psIn.posW = posW.xyz / posW.w;
    float4x4 normalMat = transpose(_Obj.modelInv);
    psIn.norW = mul((float3x3)normalMat, vsIn.nor);
    psIn.uv = vsIn.uv;
    return psIn;
}

float4 PSMain(PS_INPUT psIn) : SV_Target
{
    float3 n = normalize(psIn.norW);
    Frame3 frame = make_frame(n);

    float3 wo_w = normalize(_Camera.posW - psIn.posW);

    PointLightParams light;
    light.position = float3(3.0, 7.0, 0.5);
    light.intensity = (float3)25;

    float3 Li = eval_point_light(light, psIn.posW);
    float3 wi_w = normalize(light.position - psIn.posW);

    float3 wi = to_local(frame, wi_w);
    float3 wo = to_local(frame, wo_w);

    ConductorParams cp;
    cp.eta = float3(0.2, 0.92, 1.1);
    cp.k = float3(3.9, 2.5, 2.4);
    cp.roughness = 0.25;

    float3 brdf = conductor_brdf(wi, wo, cp);
    float3 color = brdf * Li;

    float3 srgb = linear_to_srgb(color);

    return float4(srgb, 1.0);
}
