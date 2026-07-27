// error_pass.hlsl —— 入口 shader (entry point)。
//
// 用途: 材质/shader 解析失败时的兜底 pass, 用洋红纯色标出出问题的物体。
// 入口: VSMain (Vertex), PSMain (Pixel)。
//
// 绑定: 仅 gPerObject (group 0) + gView (group 1), 无材质绑定 —— 材质正是失败的那一环,
// 兜底 pass 不能再依赖它。

#include <forward_pipeline/view.hlsli>

struct VertexInput {
    float3 Position : POSITION0;
};

struct VertexOutput {
    float4 Position : SV_Position;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    float4 position_world = mul(gPerObject.ObjectToWorld, float4(input.Position, 1.0f));
    output.Position = mul(gView.ViewProj, position_world);
    return output;
}

float4 PSMain() : SV_Target0 {
    return float4(1.0f, 0.0f, 1.0f, 1.0f);  // 洋红 = "这里的材质坏了"
}
