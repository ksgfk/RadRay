// shadow_pass.hlsl —— 入口 shader (entry point)。
//
// 用途: 阴影深度生成 pass (shadow caster, depth-only)。
// 入口: VSMain (Vertex), PSMain (Pixel, 空实现, 深度由硬件写入)。
//
// 点光源在支持 layered VS output 的设备上以 6 个 instance 一次写满 cube 六层;
// 方向光与能力 fallback 走 ViewProj[0], 按 slice 分别录制。
//
// 绑定 (编号定义见 forward_pipeline/bindings.hlsli):
//   group 0: gPerObject (b1)   每 draw 的 ObjectToWorld
//   group 1: gShadowView (b0)  每面/每级联一组 ViewProj
// 无材质绑定, 无 color target。

#pragma radray_keyword_group(PointShadowLayered, _POINT_SHADOW_LAYERED) stages(Vertex)

#include <forward_pipeline/bindings.hlsli>
#include <shadow/cube.hlsli>

struct VertexInput {
    float3 Position : POSITION0;
    float3 Normal : NORMAL0;
    float2 TexCoord : TEXCOORD0;
};

struct VertexOutput {
    float4 Position : SV_Position;
#ifdef _POINT_SHADOW_LAYERED
    uint Layer : SV_RenderTargetArrayIndex;
#endif
};

/// per-view 常量。始终保留 6 个矩阵, 使 layered 与非 layered 变体共用同一 descriptor range。
struct ShadowViewConstants {
    float4x4 ViewProj[RADRAY_CUBE_FACE_COUNT];
};

RADRAY_FORWARD_VIEW_CBUFFER(ShadowViewConstants, gShadowView, 0, 0);

VertexOutput VSMain(VertexInput input, uint instance_id : SV_InstanceID) {
    VertexOutput output;
    uint view_index = 0u;
#ifdef _POINT_SHADOW_LAYERED
    // 一次 draw 6 instance: instance 序号即 cube 面序号, 也是目标层。
    view_index = instance_id;
    output.Layer = view_index;
#endif
    float4 position_world = mul(gPerObject.ObjectToWorld, float4(input.Position, 1.0f));
    output.Position = mul(gShadowView.ViewProj[view_index], position_world);
    return output;
}

// depth-only: 无颜色输出, 深度由硬件从 SV_Position 自动写入。
void PSMain() {
}
