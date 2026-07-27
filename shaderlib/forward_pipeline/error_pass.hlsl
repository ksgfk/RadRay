// error_pass.hlsl —— 入口 shader (entry point)。
//
// 用途: 材质/shader 解析失败时的兜底 pass, 用洋红纯色标出问题物体。
// 入口: VSMain (Vertex), PSMain (Pixel)。
// 绑定: 仅 gPerObject + gView (见 forward_interface.hlsl), 无材质绑定。

#include "forward_pipeline/forward_interface.hlsl"

struct VertexInput {
    float3 Position : POSITION0;
};

struct VertexOutput {
    float4 Position : SV_Position;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    float4 worldPosition = mul(gPerObject.ObjectToWorld, float4(input.Position, 1.0));
    output.Position = mul(gView.ViewProj, worldPosition);
    return output;
}

float4 PSMain() : SV_Target0 {
    return float4(1.0, 0.0, 1.0, 1.0);
}
