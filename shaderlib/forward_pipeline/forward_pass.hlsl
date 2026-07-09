// forward_pass.hlsl: ForwardPipeline 默认前向管线的统一着色 pass。
// 点光源 + Mitsuba3 风格 Principled BRDF (复用 shaderlib/principled.hlsl + light.hlsl),
// 支持一盏点光源的立方体阴影 (point_shadow.hlsl)。
//
// 统一了原先的 forward / gltf_standard 两份着色: 顶点始终带 TANGENT (法线贴图需要),
// 贴图/自发光/AO 全部走 keyword 编译期分支, 无贴图时 (如程序化几何) 退化为纯常量材质。
// 方向约定严格遵循 principled.hlsl 顶部注释:
//   wi = 视线方向(指向相机), wo = 光源方向(指向光源), 均在着色局部坐标系 (n = +Z)。
//
// 绑定分配 (与 MeshPassExecutor + MaterialAsset 机制对齐):
//   - gMaterial (push_constant): per-material 常量 (MaterialAsset::SetConstantBlock 写)。
//   - gView (b0, space1):        per-view cbuffer (ForwardPipeline::SetViewConstants 写)。
//   - gPerObject (b1, space1):   per-object cbuffer (MeshPassExecutor 写 ObjectToWorld)。
//   - gShadowCube(t0)/gShadowSampler(s0): 管线级全局资源 (ForwardPipeline 每 draw 绑定)。
//   - 贴图/采样器 (space1, t1.. / s1): keyword 存在时声明。
//
// shader 变体 (keyword, 全 #ifdef 编译期分支, 不用 cbuffer 值做运行期 if):
//   贴图存在性: _BASECOLOR_MAP / _METALROUGHNESS_MAP / _NORMAL_MAP / _OCCLUSION_MAP / _EMISSIVE_MAP
//   alpha/双面: _ALPHATEST_ON (clip cutoff) / _ALPHABLEND_ON (输出 alpha) / _DOUBLESIDED_ON (背面法线翻转)
#include "common.hlsl"
#include "principled.hlsl"
#include "light.hlsl"
#include "point_shadow.hlsl"

struct VertexInput {
    float3 Position : POSITION0;
    float3 Normal : NORMAL0;
    float2 TexCoord : TEXCOORD0;
    float4 Tangent : TANGENT0;  // xyz 切线, w 手性符号
};

struct VertexOutput {
    float4 Position : SV_Position;
    float3 WorldPosition : POSITION0;
    float3 WorldNormal : NORMAL0;
    float2 TexCoord : TEXCOORD0;
    float4 WorldTangent : TANGENT0;  // xyz 世界切线, w 手性
};

// per-view 常量 (b0, space1)。列主序,与 Eigen / CPU 端一致。
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

// per-material 常量 (push_constant)。数值参数走 cbuffer; 分支走 keyword。
struct MaterialConstants {
    float4 BaseColor;    // rgb 基础色 (= glTF baseColorFactor), a 不透明度
    float4 Pbr;          // x metallic, y roughness, z alphaCutoff, w normalScale
    float4 Emissive;     // rgb 自发光 (已乘 strength), w occlusionStrength
    float4 Principled0;  // x specular, y specular tint, z clearcoat, w clearcoat gloss
    float4 Principled1;  // x sheen, y sheen tint, z anisotropic, w flatness
    float4 Principled2;  // x spec trans, y eta, zw 保留
};

// gMaterial 作为 push/root constant, 必须落在 b0/space0 (声明在最前且不带 register):
//   - Vulkan: VK_PUSH_CONSTANT 标记为 push constant; 与显式 register 冲突时 DXC 会静默降级,
//     故不写 register, 靠声明顺序让 DXC 自动分配到 b0/space0。
//   - D3D12: 后端把 b0/space0 的首个无显式 register cbuffer 识别为 root constant。
VK_PUSH_CONSTANT ConstantBuffer<MaterialConstants> gMaterial;
VK_BINDING(0, 1) ConstantBuffer<ViewConstants> gView : register(b0, space1);
VK_BINDING(1, 1) ConstantBuffer<PerObject> gPerObject : register(b1, space1);

// 点光源立方体阴影图 + 比较采样器 (由 ForwardPipeline 作为管线级全局资源每 draw 绑定)。
// _POINT_SHADOWS keyword (对应 URP 的 _MAIN_LIGHT_SHADOWS multi_compile): 本帧无任何投影阴影
// 的点光源时该 keyword 关闭, 阴影图绑定 + 采样代码整块从变体里剔除, 省去无谓的资源绑定与 ALU。
#ifdef _POINT_SHADOWS
VK_BINDING(2, 1) TextureCube<float> gShadowCube : register(t0, space1);
VK_BINDING(3, 1) SamplerComparisonState gShadowSampler : register(s0, space1);
#endif

// 贴图 / 采样器 (space1, t1.. 避开阴影占用的 t0; 采样器 s1 避开 s0)。
#ifdef _BASECOLOR_MAP
VK_BINDING(4, 1) Texture2D gBaseColorMap : register(t1, space1);
#endif
#ifdef _METALROUGHNESS_MAP
VK_BINDING(5, 1) Texture2D gMetalRoughMap : register(t2, space1);
#endif
#ifdef _NORMAL_MAP
VK_BINDING(6, 1) Texture2D gNormalMap : register(t3, space1);
#endif
#ifdef _OCCLUSION_MAP
VK_BINDING(7, 1) Texture2D gOcclusionMap : register(t4, space1);
#endif
#ifdef _EMISSIVE_MAP
VK_BINDING(8, 1) Texture2D gEmissiveMap : register(t5, space1);
#endif
#if defined(_BASECOLOR_MAP) || defined(_METALROUGHNESS_MAP) || defined(_NORMAL_MAP) || defined(_OCCLUSION_MAP) || defined(_EMISSIVE_MAP)
#define RR_HAS_ANY_TEXTURE 1
VK_BINDING(9, 1) SamplerState gSampler : register(s1, space1);
#endif

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    float4 worldPos = mul(gPerObject.ObjectToWorld, float4(input.Position, 1.0));
    output.Position = mul(gView.ViewProj, worldPos);
    output.WorldPosition = worldPos.xyz;
    output.WorldNormal = mul(gPerObject.ObjectToWorld, float4(input.Normal, 0.0)).xyz;
    output.TexCoord = input.TexCoord;
    float3 worldTangent = mul(gPerObject.ObjectToWorld, float4(input.Tangent.xyz, 0.0)).xyz;
    output.WorldTangent = float4(worldTangent, input.Tangent.w);
    return output;
}

float4 PSMain(VertexOutput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0 {
    // ── base color ──
    float4 baseColor = gMaterial.BaseColor;
#ifdef _BASECOLOR_MAP
    baseColor *= gBaseColorMap.Sample(gSampler, input.TexCoord);
#endif

    float alpha = 1.0f;
#ifdef _ALPHABLEND_ON
    alpha = saturate(baseColor.a);
#endif
#ifdef _ALPHATEST_ON
    // alpha cutoff: 编译期分支。阈值 (数值) 走 cbuffer, "是否裁剪"由 keyword 决定。
    clip(saturate(baseColor.a) - saturate(gMaterial.Pbr.z));
#endif

    // ── metallic-roughness ──
    float metallic = saturate(gMaterial.Pbr.x);
    float roughness = saturate(gMaterial.Pbr.y);
#ifdef _METALROUGHNESS_MAP
    // glTF 约定: G = roughness, B = metallic。
    float2 mr = gMetalRoughMap.Sample(gSampler, input.TexCoord).gb;
    roughness *= mr.x;
    metallic *= mr.y;
#endif
    roughness = max(roughness, 0.001f);

    // ── 法线 (TBN 空间法线贴图) ──
    float3 n = normalize(input.WorldNormal);
#ifdef _DOUBLESIDED_ON
    // 双面着色: 背面把插值法线翻向相机一侧, 使内壁也能正常受光。
    // (背面能否被光栅化取决于 PSO CullMode = None, 由 pass 固定状态保证。)
    if (!isFrontFace) {
        n = -n;
    }
#endif
#ifdef _NORMAL_MAP
    {
        float3 t = normalize(input.WorldTangent.xyz - n * dot(input.WorldTangent.xyz, n));
        float3 b = cross(n, t) * input.WorldTangent.w;
        float3 sampled = gNormalMap.Sample(gSampler, input.TexCoord).xyz * 2.0f - 1.0f;
        sampled.xy *= gMaterial.Pbr.w;  // normalScale
        n = normalize(t * sampled.x + b * sampled.y + n * sampled.z);
    }
#endif

    // ── occlusion ──
    float occlusion = 1.0f;
#ifdef _OCCLUSION_MAP
    float occSample = gOcclusionMap.Sample(gSampler, input.TexCoord).r;
    occlusion = lerp(1.0f, occSample, saturate(gMaterial.Emissive.w));
#endif

    // ── emissive ──
    float3 emissive = gMaterial.Emissive.rgb;
#ifdef _EMISSIVE_MAP
    emissive *= gEmissiveMap.Sample(gSampler, input.TexCoord).rgb;
#endif

    // ── Principled 参数 ──
    float3 albedo = saturate(baseColor.rgb);
    float specular = saturate(gMaterial.Principled0.x);
    float specTint = saturate(gMaterial.Principled0.y);
    float clearcoat = saturate(gMaterial.Principled0.z);
    float clearcoatGloss = saturate(gMaterial.Principled0.w);
    float sheen = saturate(gMaterial.Principled1.x);
    float sheenTint = saturate(gMaterial.Principled1.y);
    float anisotropic = saturate(gMaterial.Principled1.z);
    float flatness = saturate(gMaterial.Principled1.w);
    float specTrans = saturate(gMaterial.Principled2.x);
    float eta = max(gMaterial.Principled2.y, 1.001f);

    float3 viewDirWorld = normalize(gView.CameraPosition.xyz - input.WorldPosition);
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
#ifdef _POINT_SHADOWS
    // 投影阴影的点光源序号 (0 = 无阴影, 否则实际序号 = shadowIndex1 - 1)。
    // 仅在 _POINT_SHADOWS 变体存在; 无阴影变体里整块阴影采样被剔除。
    uint shadowIndex1 = gView.LightCounts.y;
#endif
    for (uint j = 0; j < ptCount; ++j) {
        PointLightGpu L = gView.PointLights[j];
        float3 woW = normalize(L.Position.xyz - input.WorldPosition);
        float3 wo = to_local(frame, woW);
        if (wo.z <= 0.0f) {
            continue;
        }
        float3 Li = eval_point_irradiance(L, input.WorldPosition);
#ifdef _POINT_SHADOWS
        // 该光源投影阴影: 用 cube 阴影图采样可见度, 乘到辐照度上。
        if (shadowIndex1 != 0u && (j + 1u) == shadowIndex1) {
            float visibility = sample_point_shadow(
                gShadowCube, gShadowSampler, gView.PointShadow,
                input.WorldPosition, n);
            Li *= visibility;
        }
#endif
        Lo += EvalPrincipledReflection(
                  normalize(wi), normalize(wo), albedo, metallic, roughness,
                  specular, specTint, anisotropic, sheen,
                  sheenTint, flatness, clearcoat, clearcoatGloss,
                  specTrans, eta) *
              Li;
    }

    float3 color = Lo * occlusion + emissive;
    color = color / (color + 1.0f.xxx);  // Reinhard tone map
    color = linear_to_srgb(saturate(color));
    return float4(color, alpha);
}
