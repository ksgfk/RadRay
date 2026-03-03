#include <common.hlsl>

struct ObjectData
{
    float4x4 model;
    float4x4 mvp;
    float4x4 modelInv;
    float4 baseColor;
    float roughness;
    float metallic;
    float2 _pad0;
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

struct GBufferOutput
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Material : SV_Target2;
};

VK_PUSH_CONSTANT ConstantBuffer<ObjectData> _Obj : register(b0);

PS_INPUT VSMain(VS_INPUT input)
{
    PS_INPUT output;
    output.pos = mul(_Obj.mvp, float4(input.pos, 1.0));
    float4 posW = mul(_Obj.model, float4(input.pos, 1.0));
    output.posW = posW.xyz / max(posW.w, 1e-6);
    float3x3 normalMat = (float3x3)transpose(_Obj.modelInv);
    output.norW = mul(normalMat, input.nor);
    output.uv = input.uv;
    return output;
}

GBufferOutput PSMain(PS_INPUT input)
{
    GBufferOutput output;
    float3 n = normalize(input.norW);
    output.Albedo = float4(saturate(_Obj.baseColor.rgb), 1.0);
    output.Normal = float4(n * 0.5 + 0.5, 1.0);
    output.Material = float4(saturate(_Obj.roughness), saturate(_Obj.metallic), 0.0, 1.0);
    return output;
}
