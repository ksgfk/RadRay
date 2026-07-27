// shadow_pass.hlsl —— 入口 shader (entry point)。
//
// 用途: 阴影深度生成 pass (shadow caster, depth-only)。
// 入口: VSMain (Vertex), PSMain (Pixel, 空实现, 仅写深度)。
//
// 点光源在支持 layered VS output 的设备上以 6 个 instance 一次写入 cube 六层；
// 方向光与能力 fallback 使用 ViewProj[0] 按 slice 录制。
//
// 绑定约定 (CPU 端靠 cbuffer 名字定位):
//   - gShadowView (b0, space1, per-view):   每面/每级联写入一组 ViewProj。
//   - gPerObject  (b1, space0, per-object): 每 draw 写入 ObjectToWorld (见 binding_abi.hlsl)。
// 无 material 绑定, 无 color target。

#include "common.hlsl"
#include "forward_pipeline/binding_abi.hlsl"

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

// per-view 常量统一保留 6 个矩阵，使 layered/base variant 共用相同 descriptor range。
struct ShadowViewConstants {
    float4x4 ViewProj[6];
};

VK_BINDING(0, RADRAY_FORWARD_PIPELINE_BINDING_GROUP)
ConstantBuffer<ShadowViewConstants> gShadowView : register(b0, RADRAY_FORWARD_PIPELINE_SPACE);

VertexOutput VSMain(VertexInput input, uint instanceId : SV_InstanceID) {
    VertexOutput output;
    uint viewIndex = 0;
#ifdef _POINT_SHADOW_LAYERED
    viewIndex = instanceId;
    output.Layer = viewIndex;
#endif
    float4 worldPos = mul(gPerObject.ObjectToWorld, float4(input.Position, 1.0));
    output.Position = mul(gShadowView.ViewProj[viewIndex], worldPos);
    return output;
}

// depth-only: 无颜色输出, 深度由硬件从 SV_Position 自动写入。
void PSMain() {
}
