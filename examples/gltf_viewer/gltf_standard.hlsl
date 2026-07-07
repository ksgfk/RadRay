// gltf_standard.hlsl: glTF 2.0 metallic-roughness 标准材质前向渲染。
// 点光源 + Mitsuba3 风格 Principled BRDF (复用 shaderlib/principled.hlsl)。
//
// 绑定 (对齐 material.hlsl 约定):
//   - gMaterial (push_constant): per-material 常量 (MaterialAsset::SetConstantBlock 写)。
//   - gView (b0, space1):        per-view cbuffer (ForwardPipeline::SetViewConstants 写)。
//   - gPerObject (b1, space1):   per-object cbuffer (MeshPassExecutor 写 ObjectToWorld)。
//   - 贴图/采样器 (space1):      RADRAY_MATERIAL_TEXTURE2D / SAMPLER 声明。
//
// shader 变体 (keyword, 全 #ifdef 编译期分支, 不用 cbuffer 值做运行期 if):
//   贴图存在性: _BASECOLOR_MAP / _METALROUGHNESS_MAP / _NORMAL_MAP / _OCCLUSION_MAP / _EMISSIVE_MAP
//   alpha/双面: _ALPHATEST_ON (clip cutoff) / _ALPHABLEND_ON (输出 alpha) / _DOUBLESIDED_ON (背面法线翻转)
//
// 贴图槽 (与 gltf_viewer MaterialFactory 的 SetTexture 名字一致):
//   gBaseColorMap(t0) gMetalRoughMap(t1) gNormalMap(t2) gOcclusionMap(t3) gEmissiveMap(t4)
//   gSampler(s0) 全槽共享。
#include "common.hlsl"
#include "principled.hlsl"
#include "light.hlsl"

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

struct ViewConstants {
    float4x4 ViewProj;
    float4 CameraPosition;
    uint4 LightCounts;  // x = point light count
    PointLightGpu PointLights[RR_MAX_POINT_LIGHTS];
};

struct PerObject {
    float4x4 ObjectToWorld;
};

// per-material 常量。数值参数走 cbuffer; 分支走 keyword。
struct MaterialConstants {
    float4 BaseColorFactor;  // rgb 基础色, a 不透明度
    float4 MetalRough;       // x metallic, y roughness, z alphaCutoff, w normalScale
    float4 Emissive;         // rgb 自发光 (已乘 strength), w occlusionStrength
    float4 Principled0;      // x specular, y specularTint, z clearcoat, w clearcoatGloss
    float4 Principled1;      // x sheen, y sheenTint, zw 保留
};

VK_PUSH_CONSTANT ConstantBuffer<MaterialConstants> gMaterial;
VK_BINDING(0, 1) ConstantBuffer<ViewConstants> gView : register(b0, space1);
VK_BINDING(1, 1) ConstantBuffer<PerObject> gPerObject : register(b1, space1);

// 贴图 / 采样器 (space1, binding 从 2 起, 避开 cbuffer 占用的 0/1)。
#ifdef _BASECOLOR_MAP
VK_BINDING(2, 1) Texture2D gBaseColorMap : register(t0, space1);
#endif
#ifdef _METALROUGHNESS_MAP
VK_BINDING(3, 1) Texture2D gMetalRoughMap : register(t1, space1);
#endif
#ifdef _NORMAL_MAP
VK_BINDING(4, 1) Texture2D gNormalMap : register(t2, space1);
#endif
#ifdef _OCCLUSION_MAP
VK_BINDING(5, 1) Texture2D gOcclusionMap : register(t3, space1);
#endif
#ifdef _EMISSIVE_MAP
VK_BINDING(6, 1) Texture2D gEmissiveMap : register(t4, space1);
#endif
#if defined(_BASECOLOR_MAP) || defined(_METALROUGHNESS_MAP) || defined(_NORMAL_MAP) || defined(_OCCLUSION_MAP) || defined(_EMISSIVE_MAP)
#define RR_HAS_ANY_TEXTURE 1
VK_BINDING(7, 1) SamplerState gSampler : register(s0, space1);
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
    float4 baseColor = gMaterial.BaseColorFactor;
#ifdef _BASECOLOR_MAP
    baseColor *= gBaseColorMap.Sample(gSampler, input.TexCoord);
#endif

    float alpha = 1.0f;
#ifdef _ALPHABLEND_ON
    alpha = saturate(baseColor.a);
#endif
#ifdef _ALPHATEST_ON
    // alpha cutoff: 编译期分支。阈值 (数值) 走 cbuffer, "是否裁剪"由 keyword 决定。
    clip(baseColor.a - gMaterial.MetalRough.z);
#endif

    // ── metallic-roughness ──
    float metallic = saturate(gMaterial.MetalRough.x);
    float roughness = saturate(gMaterial.MetalRough.y);
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
    if (!isFrontFace) {
        n = -n;
    }
#endif
#ifdef _NORMAL_MAP
    {
        float3 t = normalize(input.WorldTangent.xyz - n * dot(input.WorldTangent.xyz, n));
        float3 b = cross(n, t) * input.WorldTangent.w;
        float3 sampled = gNormalMap.Sample(gSampler, input.TexCoord).xyz * 2.0f - 1.0f;
        sampled.xy *= gMaterial.MetalRough.w;  // normalScale
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

    // ── Principled 参数 (metallic-roughness 映射) ──
    float3 albedo = saturate(baseColor.rgb);
    float specular = saturate(gMaterial.Principled0.x);
    float specTint = saturate(gMaterial.Principled0.y);
    float clearcoat = saturate(gMaterial.Principled0.z);
    float clearcoatGloss = saturate(gMaterial.Principled0.w);
    float sheen = saturate(gMaterial.Principled1.x);
    float sheenTint = saturate(gMaterial.Principled1.y);
    const float anisotropic = 0.0f;
    const float flatness = 0.0f;
    const float specTrans = 0.0f;
    const float eta = 1.5f;

    float3 viewDirWorld = normalize(gView.CameraPosition.xyz - input.WorldPosition);
    Frame3 frame = make_frame(n);
    float3 wi = to_local(frame, viewDirWorld);
#ifndef _DOUBLESIDED_ON
    if (wi.z <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, alpha);
    }
#endif

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
