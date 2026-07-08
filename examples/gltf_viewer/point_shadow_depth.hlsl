// 点光源立方体阴影的深度生成 shader (shadow caster, depth-only)。
//
// 每个 cube 面各录制一次: ForwardPipeline 每面用 SetViewConstants 写入该面的
// 世界->裁剪 矩阵 (gShadowView.ViewProj), MeshPassExecutor 每 draw 写入 gPerObject.ObjectToWorld。
// 光栅化后由硬件把 SV_Position.z (标准深度, near=0 far=1) 写入 cube 对应面的深度 slice。
//
// 绑定约定与 forward.hlsl / MeshPassExecutor 对齐 (靠 cbuffer 名字被 FindParameterId 命中):
//   - gShadowView (per-view): 由 ForwardPipeline::SetViewConstants 每面写入。
//   - gPerObject  (per-object): 由 MeshPassExecutor 每 draw 写入 ObjectToWorld。
// depth-only, 无 material push constant, 无 color target。PSMain 为空 (仅写深度)。

#include "common.hlsl"

struct VertexInput {
    float3 Position : POSITION0;
    float3 Normal : NORMAL0;
    float2 TexCoord : TEXCOORD0;
};

struct VertexOutput {
    float4 Position : SV_Position;
};

// per-view (单面) 常量: 世界 -> 裁剪。列主序, 与 Eigen / CPU 端一致。
struct ShadowViewConstants {
    float4x4 ViewProj;
};

// per-object 常量: 执行器写入 ObjectToWorld。
struct PerObject {
    float4x4 ObjectToWorld;
};

VK_BINDING(0, 1) ConstantBuffer<ShadowViewConstants> gShadowView : register(b0, space1);
VK_BINDING(1, 1) ConstantBuffer<PerObject> gPerObject : register(b1, space1);

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    float4 worldPos = mul(gPerObject.ObjectToWorld, float4(input.Position, 1.0));
    output.Position = mul(gShadowView.ViewProj, worldPos);
    return output;
}

// depth-only: 无颜色输出, 深度由硬件从 SV_Position 自动写入。
void PSMain(VertexOutput input) {
    (void)input;
}
