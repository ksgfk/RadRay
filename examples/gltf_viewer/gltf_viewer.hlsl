#include "common.hlsl"

struct VertexInput {
    VK_LOCATION(0) float3 Position : POSITION0;
    VK_LOCATION(1) float3 Normal : NORMAL0;
    VK_LOCATION(2) float2 TexCoord : TEXCOORD0;
    VK_LOCATION(3) float4 Tangent : TANGENT0;
};

struct VertexOutput {
    float4 Position : SV_Position;
    float3 WorldNormal : NORMAL0;
};

struct SceneConstants {
    float4x4 MVP;
    float4x4 Model;
};

VK_PUSH_CONSTANT ConstantBuffer<SceneConstants> gScene : register(b0, space0);

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    output.Position = mul(gScene.MVP, float4(input.Position, 1.0));
    output.WorldNormal = mul(gScene.Model, float4(input.Normal, 0.0)).xyz;
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target0 {
    float3 n = normalize(input.WorldNormal);
    float3 lightDir = normalize(float3(0.35, 0.85, 0.4));
    float ndotl = saturate(dot(n, lightDir));
    float3 normalColor = n * 0.5 + 0.5;
    float3 base = lerp(float3(0.18, 0.20, 0.24), normalColor, 0.65);
    float3 color = base * (0.25 + 0.75 * ndotl);
    return float4(linear_to_srgb(saturate(color)), 1.0);
}
