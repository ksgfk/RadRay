// 最小前向渲染 shader: 点光源 + Mitsuba3 风格 Principled BRDF。
// 组合 shaderlib 的 principled.hlsl (BRDF) 与 light.hlsl (点光源辐照度)。
// 方向约定严格遵循 principled.hlsl 顶部注释:
//   wi = 视线方向(指向相机),wo = 光源方向(指向光源),均在着色局部坐标系 (n = +Z)。
//
// 绑定分配 (与 MeshPassExecutor + MaterialAsset 机制对齐):
//   - gView (b0, space0):     per-view cbuffer, 由 ForwardPipeline::SetViewConstants 每帧写入。
//   - gPerObject (b1, space1): per-object cbuffer, 由 MeshPassExecutor 每 draw 写入 ObjectToWorld。
//   - gMaterial (push_constant): per-material 常量, 由 MaterialAsset::SetConstantBlock 写入。
#include "common.hlsl"
#include "principled.hlsl"
#include "light.hlsl"

struct VertexInput {
    float3 Position : POSITION0;
    float3 Normal : NORMAL0;
    float2 TexCoord : TEXCOORD0;
};

struct VertexOutput {
    float4 Position : SV_Position;
    float3 WorldPosition : POSITION0;
    float3 WorldNormal : NORMAL0;
    float2 TexCoord : TEXCOORD0;
};

// per-view 常量 (b0, space0)。列主序,与 Eigen / CPU 端一致。
// 灯光以定长数组内联 (arena 缓冲不支持 StructuredBuffer, 故用 cbuffer 数组)。
struct ViewConstants {
    float4x4 ViewProj;      // 世界 -> 裁剪
    float4 CameraPosition;  // xyz 相机世界位置
    uint4 LightCounts;      // x = point light count
    PointLightGpu PointLights[RR_MAX_POINT_LIGHTS];
};

// per-object 常量 (b1, space1)。执行器写入 ObjectToWorld。
struct PerObject {
    float4x4 ObjectToWorld;
};

// per-material Principled 参数 (push_constant)。
struct MaterialConstants {
    float4 BaseColor;    // rgb 基础色
    float4 Principled0;  // x metallic, y roughness, z specular, w specular tint
    float4 Principled1;  // x anisotropic, y sheen, z sheen tint, w flatness
    float4 Principled2;  // x clearcoat, y clearcoat gloss, z spec trans, w eta
};

// gMaterial 作为 push/root constant, 必须落在 b0/space0:
//   - Vulkan: [[vk::push_constant]] 标记为 push constant; 此标记与显式 register 冲突时 DXC
//     会静默把它降级为普通 uniform cbuffer, 故此处 *不写* register, 靠声明顺序让 DXC 自动分配到 b0/space0。
//   - D3D12: 后端把 b0/space0 的 cbuffer 识别为 root constant (见 d3d12_binding_layout 的候选规则),
//     DXC 按声明顺序自动把首个无显式 register 的 cbuffer 分配到 b0/space0。
// 因此 gMaterial 必须声明在最前, 且不带 register。
VK_PUSH_CONSTANT ConstantBuffer<MaterialConstants> gMaterial;
VK_BINDING(0, 1) ConstantBuffer<ViewConstants> gView : register(b0, space1);
VK_BINDING(1, 1) ConstantBuffer<PerObject> gPerObject : register(b1, space1);

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    float4 worldPos = mul(gPerObject.ObjectToWorld, float4(input.Position, 1.0));
    output.Position = mul(gView.ViewProj, worldPos);
    output.WorldPosition = worldPos.xyz;
    output.WorldNormal = mul(gPerObject.ObjectToWorld, float4(input.Normal, 0.0)).xyz;
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target0 {
    float3 n = normalize(input.WorldNormal);
    float3 viewDirWorld = normalize(gView.CameraPosition.xyz - input.WorldPosition);
    if (dot(n, viewDirWorld) < 0.0f) {
        n = -n;
    }

    float3 baseColor = saturate(gMaterial.BaseColor.rgb);
    float metallic = saturate(gMaterial.Principled0.x);
    float roughness = max(saturate(gMaterial.Principled0.y), 0.001f);
    float specular = saturate(gMaterial.Principled0.z);
    float specTint = saturate(gMaterial.Principled0.w);
    float anisotropic = saturate(gMaterial.Principled1.x);
    float sheen = saturate(gMaterial.Principled1.y);
    float sheenTint = saturate(gMaterial.Principled1.z);
    float flatness = saturate(gMaterial.Principled1.w);
    float clearcoat = saturate(gMaterial.Principled2.x);
    float clearcoatGloss = saturate(gMaterial.Principled2.y);
    float specTrans = saturate(gMaterial.Principled2.z);
    float eta = max(gMaterial.Principled2.w, 1.001f);

    Frame3 frame = make_frame(n);
    float3 wi = to_local(frame, viewDirWorld);
    if (wi.z <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    float3 Lo = float3(0.0f, 0.0f, 0.0f);
    uint ptCount = min(gView.LightCounts.x, (uint)RR_MAX_POINT_LIGHTS);
    for (uint j = 0; j < ptCount; ++j) {
        PointLightGpu L = gView.PointLights[j];
        float3 woW = normalize(L.Position.xyz - input.WorldPosition);
        float3 wo = to_local(frame, woW);
        if (wo.z <= 0.0f) {
            continue;
        }
        float3 Li = eval_point_irradiance(L, input.WorldPosition);
        Lo += EvalPrincipledReflection(
                  normalize(wi), normalize(wo), baseColor, metallic, roughness,
                  specular, specTint, anisotropic, sheen,
                  sheenTint, flatness, clearcoat, clearcoatGloss,
                  specTrans, eta) *
              Li;
    }

    float3 color = Lo / (Lo + 1.0f.xxx);  // Reinhard tone map
    color = linear_to_srgb(saturate(color));
    return float4(color, 1.0f);
}
