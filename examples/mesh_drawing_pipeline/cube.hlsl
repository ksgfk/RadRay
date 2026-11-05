#include <core/platform.hlsli>

struct PassConstants {
    column_major float4x4 ViewProjection;
    float LocalScale;
    float3 Padding;
};

struct MaterialPushData {
    float4 Color;
};

VK_BINDING(0, 0) ConstantBuffer<PassConstants> Pass : register(b0, space0);
VK_PUSH_CONSTANT ConstantBuffer<MaterialPushData> MaterialPush : register(b1, space0);

struct VSInput {
    VK_LOCATION(0) float3 Position : POSITION;
    VK_LOCATION(1) float4 InstCol0 : INST_COL0;
    VK_LOCATION(2) float4 InstCol1 : INST_COL1;
    VK_LOCATION(3) float4 InstCol2 : INST_COL2;
    VK_LOCATION(4) float4 InstCol3 : INST_COL3;
};

[shader("vertex")]
float4 VSMain(VSInput input) : SV_Position {
    float3 scaled = input.Position * Pass.LocalScale;
    float4 worldPosition = input.InstCol0 * scaled.x + input.InstCol1 * scaled.y + input.InstCol2 * scaled.z + input.InstCol3;
    return mul(Pass.ViewProjection, worldPosition);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return MaterialPush.Color;
}
