// shadow_pass.hlsl: 点光源立方体阴影的深度生成 pass (shadow caster, depth-only)。
//
// 点光源在支持 layered VS output 的设备上以 6 个 instance 一次写入 cube 六层；
// 方向光与能力 fallback 使用 ViewProj[0] 按 slice 录制。
//
// 绑定约定与 forward_pass.hlsl / MeshPassExecutor 对齐 (靠 cbuffer 名字被 FindParameterId 命中):
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
#ifdef _POINT_SHADOW_LAYERED
    uint Layer : SV_RenderTargetArrayIndex;
#endif
};

// per-view 常量统一保留 6 个矩阵，使 layered/base variant 共用相同 descriptor range。
struct ShadowViewConstants {
    float4x4 ViewProj[6];
};

// per-object 常量: 执行器写入 ObjectToWorld。
struct PerObject {
    float4x4 ObjectToWorld;
};

VK_BINDING(1, 0) ConstantBuffer<PerObject> gPerObject : register(b1, space0);
VK_BINDING(0, 1) ConstantBuffer<ShadowViewConstants> gShadowView : register(b0, space1);

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
void PSMain(VertexOutput input) {
    (void)input;
}
