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
    float4x4 MVP;    // model-view-projection (column-major, matches Eigen storage)
    float4x4 Model;  // model rotation used to bring normals into world space
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
    // Map normal from [-1, 1] to [0, 1] so each axis becomes an RGB channel.
    float3 color = n * 0.5 + 0.5;
    return float4(color, 1.0);
}
