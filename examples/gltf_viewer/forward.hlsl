// 最小前向渲染 shader: 点光源 + Mitsuba3 风格 Principled BRDF。
// 组合 shaderlib 的 principled.hlsl (BRDF) 与 light.hlsl (点光源辐照度)。
// 方向约定严格遵循 principled.hlsl 顶部注释:
//   wi = 视线方向(指向相机),wo = 光源方向(指向光源),均在着色局部坐标系 (n = +Z)。
//
// 绑定分配 (与 MeshPassExecutor + MaterialAsset 机制对齐):
//   - gView (b0, space0):     per-view cbuffer, 由 ForwardPipeline::SetViewConstants 每帧写入。
//   - gPerObject (b1, space1): per-object cbuffer, 由 MeshPassExecutor 每 draw 写入 ObjectToWorld。
//   - gMaterial (push_constant): per-material 常量, 由 MaterialAsset::SetConstantBlock 写入。
//
// shader 变体 (keyword, 编译期分支; 由 MaterialAsset::EnableKeyword 驱动 ShaderVariantCache):
//   - _ALPHATEST_ON:   alpha cutoff, clip(alpha - cutoff)。未定义时无任何裁剪逻辑。
//   - _DOUBLESIDED_ON: 双面着色, 用 SV_IsFrontFace 翻转背面法线, 并跳过单面 early-out。
//                      (注: 背面能否被光栅化取决于 PSO 的 CullMode, 由 pass 固定状态决定。)
//   - _ALPHABLEND_ON:  输出 BaseColor.a 作为混合 alpha (未定义时恒输出 1.0)。
//
// 这些分支一律走 #ifdef 编译期消除, 不用 cbuffer 值做运行期 if。alphaCutoff 阈值属数值参数, 走 cbuffer。
#include "common.hlsl"
#include "principled.hlsl"
#include "light.hlsl"
#include "point_shadow.hlsl"

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
    uint4 LightCounts;      // x = point light count, y = shadow point-light index+1 (0 = 无阴影)
    PointLightGpu PointLights[RR_MAX_POINT_LIGHTS];
    PointShadowData PointShadow;  // 投影阴影点光源的立方体阴影数据
};

// per-object 常量 (b1, space1)。执行器写入 ObjectToWorld。
struct PerObject {
    float4x4 ObjectToWorld;
};

// per-material Principled 参数 (push_constant)。
struct MaterialConstants {
    float4 BaseColor;    // rgb 基础色, a 不透明度 (仅 _ALPHABLEND_ON 时消费)
    float4 Principled0;  // x metallic, y roughness, z specular, w specular tint
    float4 Principled1;  // x anisotropic, y sheen, z sheen tint, w flatness
    float4 Principled2;  // x clearcoat, y clearcoat gloss, z spec trans, w eta
    float4 AlphaParams;  // x alphaCutoff (仅 _ALPHATEST_ON 时消费), yzw 保留
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

// 点光源立方体阴影图 + 比较采样器 (由 ForwardPipeline 作为管线级全局资源每 draw 绑定)。
VK_BINDING(2, 1) TextureCube<float> gShadowCube : register(t0, space1);
VK_BINDING(3, 1) SamplerComparisonState gShadowSampler : register(s0, space1);

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    float4 worldPos = mul(gPerObject.ObjectToWorld, float4(input.Position, 1.0));
    output.Position = mul(gView.ViewProj, worldPos);
    output.WorldPosition = worldPos.xyz;
    output.WorldNormal = mul(gPerObject.ObjectToWorld, float4(input.Normal, 0.0)).xyz;
    output.TexCoord = input.TexCoord;
    return output;
}

float4 PSMain(VertexOutput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0 {
    float alpha = 1.0f;
#ifdef _ALPHABLEND_ON
    alpha = saturate(gMaterial.BaseColor.a);
#endif

#ifdef _ALPHATEST_ON
    // alpha cutoff: 编译期分支, 未定义 _ALPHATEST_ON 时该 clip 完全不存在。
    // 阈值 alphaCutoff 是数值参数, 走 cbuffer; "是否裁剪"这个分支由 keyword 决定。
    float cutoff = saturate(gMaterial.AlphaParams.x);
    float maskAlpha = saturate(gMaterial.BaseColor.a);
    clip(maskAlpha - cutoff);
#endif

    float3 n = normalize(input.WorldNormal);
#ifdef _DOUBLESIDED_ON
    // 双面着色: 背面把插值法线翻向相机一侧, 使内壁也能正常受光。
    // (背面能否被光栅化取决于 PSO CullMode = None, 由 pass 固定状态保证。)
    if (!isFrontFace) {
        n = -n;
    }
#endif
    float3 viewDirWorld = normalize(gView.CameraPosition.xyz - input.WorldPosition);

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
#ifndef _DOUBLESIDED_ON
    // 单面: 掠射/背向相机的像素归零 (封闭不透明体的可见半球法线始终朝相机)。
    // 双面时法线已翻向相机, 不做此 early-out, 否则内壁会被误剔。
    if (wi.z <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, alpha);
    }
#endif

    float3 Lo = float3(0.0f, 0.0f, 0.0f);
    uint ptCount = min(gView.LightCounts.x, (uint)RR_MAX_POINT_LIGHTS);
    // 投影阴影的点光源序号 (0 = 无阴影, 否则实际序号 = shadowIndex1 - 1)。
    uint shadowIndex1 = gView.LightCounts.y;
    for (uint j = 0; j < ptCount; ++j) {
        PointLightGpu L = gView.PointLights[j];
        float3 woW = normalize(L.Position.xyz - input.WorldPosition);
        float3 wo = to_local(frame, woW);
        if (wo.z <= 0.0f) {
            continue;
        }
        float3 Li = eval_point_irradiance(L, input.WorldPosition);
        // 该光源投影阴影: 用 cube 阴影图采样可见度, 乘到辐照度上。
        if (shadowIndex1 != 0u && (j + 1u) == shadowIndex1) {
            float visibility = sample_point_shadow(
                gShadowCube, gShadowSampler, gView.PointShadow,
                input.WorldPosition, n);
            Li *= visibility;
        }
        Lo += EvalPrincipledReflection(
                  normalize(wi), normalize(wo), baseColor, metallic, roughness,
                  specular, specTint, anisotropic, sheen,
                  sheenTint, flatness, clearcoat, clearcoatGloss,
                  specTrans, eta) *
              Li;
    }

    float3 color = Lo / (Lo + 1.0f.xxx);  // Reinhard tone map
    color = linear_to_srgb(saturate(color));
    return float4(color, alpha);
}
