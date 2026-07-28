#ifndef RADRAY_FORWARD_PIPELINE_STANDARD_MATERIAL_HLSLI
#define RADRAY_FORWARD_PIPELINE_STANDARD_MATERIAL_HLSLI

#include <bsdf/principled.hlsli>
#include <core/frame.hlsli>
#include <forward_pipeline/bindings.hlsli>

// 标准 (glTF metallic-roughness + Principled 扩展) 材质的绑定与采样。
//
// 五个贴图槽与采样器【始终】声明, keyword 只控制是否采样, 绝不改变绑定 ABI ——
// 否则每个变体都会有不同的 descriptor layout, CPU 端就没法共用一套绑定代码。
//
// 依赖调用方 (入口 shader) 声明这些 keyword 组:
//   _BASECOLOR_MAP / _METALROUGHNESS_MAP / _NORMAL_MAP / _OCCLUSION_MAP / _EMISSIVE_MAP
//
// 混合与双面【不是】keyword: 它们由 MaterialRenderState 的 Blend / Cull 表达。本文件
// 无条件写出 Alpha 并按 SV_IsFrontFace 翻转法线; 混合关闭时 Alpha 被 blend state 忽略,
// CullMode::Back 下背面不进 PS 使翻转成为死代码 —— 两者都无需 keyword 守护。

/// per-material 常量。数值参数走持久 cbuffer, 分支走 keyword。
/// 字段按 float4 打包, 顺序须与 CPU 端材质写入逐字段对齐。
struct MaterialConstants {
    float4 BaseColor;    // rgb = 基础色 (glTF baseColorFactor), a = 不透明度
    float4 Pbr;          // x = metallic, y = roughness, z = alphaCutoff, w = normalScale
    float4 Emissive;     // rgb = 自发光 (已乘 strength), w = occlusionStrength
    float4 Principled0;  // x = specular, y = specularTint, z = clearcoat, w = clearcoatGloss
    float4 Principled1;  // x = sheen, y = sheenTint, z = anisotropy, w = flatness
    float4 Principled2;  // x = specularTrans, y = eta, zw 保留
};

RADRAY_FORWARD_MATERIAL_CBUFFER(MaterialConstants, gMaterial, 0, 0);

RADRAY_FORWARD_MATERIAL_TEXTURE2D(gBaseColorMap, 1, 1);
RADRAY_FORWARD_MATERIAL_TEXTURE2D(gMetalRoughMap, 2, 2);
RADRAY_FORWARD_MATERIAL_TEXTURE2D(gNormalMap, 3, 3);
RADRAY_FORWARD_MATERIAL_TEXTURE2D(gOcclusionMap, 4, 4);
RADRAY_FORWARD_MATERIAL_TEXTURE2D(gEmissiveMap, 5, 5);
RADRAY_FORWARD_MATERIAL_SAMPLER(gSampler, 6, 6);

/// 采样后的表面属性。所有贴图与常量都已合并, 值域已 clamp。
struct SurfaceSample {
    float3 Albedo;     // 线性空间基础色
    /// 不透明度。同时是 alpha test 的比较值与写入 target 的 alpha ——
    /// 混合关闭时后者被 blend state 丢弃, 故无需拆成两个字段。
    float Alpha;
    float3 Normal;     // 世界空间着色法线 (归一化, 已处理双面翻转与法线贴图)
    float Metallic;
    float Roughness;
    float Occlusion;   // 环境光遮蔽系数
    float3 Emissive;   // 线性空间自发光
};

/// 采样标准材质, 合并贴图与常量。
///   texcoord:       uv0
///   normal_world:   插值后的世界法线 (未归一化)
///   tangent_world:  xyz = 插值后的世界切线, w = 手性
///   is_front_face:  SV_IsFrontFace
SurfaceSample sample_standard_material(
    float2 texcoord,
    float3 normal_world,
    float4 tangent_world,
    bool is_front_face) {
    SurfaceSample surface;

    // ── base color / alpha ──
    float4 base_color = gMaterial.BaseColor;
#ifdef _BASECOLOR_MAP
    base_color *= gBaseColorMap.Sample(gSampler, texcoord);
#endif
    surface.Albedo = saturate(base_color.rgb);
    // 无条件写出 Alpha。不混合时这个值会被 blend state 丢弃, 故不需要 keyword 把它
    // 归一 —— 是否混合是纯固定功能决策, 由 MaterialRenderState::Blend 单独表达。
    surface.Alpha = saturate(base_color.a);

    // ── metallic / roughness ──
    surface.Metallic = saturate(gMaterial.Pbr.x);
    surface.Roughness = saturate(gMaterial.Pbr.y);
#ifdef _METALROUGHNESS_MAP
    // glTF 约定: G = roughness, B = metallic。
    float2 mr = gMetalRoughMap.Sample(gSampler, texcoord).gb;
    surface.Roughness *= mr.x;
    surface.Metallic *= mr.y;
#endif
    // 下限防止 GGX 在完美镜面处出现除零高光。
    surface.Roughness = max(surface.Roughness, 0.001f);

    // ── 法线 ──
    float3 n = safe_normalize(normal_world, float3(0.0f, 0.0f, 1.0f));
    // 背面把插值法线翻向相机一侧, 使内壁也能正常受光。
    //
    // 无条件执行: 是否双面是纯固定功能决策 (MaterialRenderState::Cull)。CullMode::Back
    // 下背面根本不进 PS, is_front_face 恒为 true, 这个分支是死代码; CullMode::None 下
    // 它才生效。用 keyword 守护它不会改变任何一条路径的 bytecode 语义。
    if (!is_front_face) {
        n = -n;
    }
#ifdef _NORMAL_MAP
    {
        Frame3 tbn = make_frame_tangent(n, tangent_world.xyz, tangent_world.w);
        float3 sampled = gNormalMap.Sample(gSampler, texcoord).xyz * 2.0f - 1.0f;
        sampled.xy *= gMaterial.Pbr.w;  // normalScale
        n = safe_normalize(frame_to_world(tbn, sampled), n);
    }
#endif
    surface.Normal = n;

    // ── occlusion ──
    surface.Occlusion = 1.0f;
#ifdef _OCCLUSION_MAP
    float occlusion = gOcclusionMap.Sample(gSampler, texcoord).r;
    surface.Occlusion = lerp(1.0f, occlusion, saturate(gMaterial.Emissive.w));
#endif

    // ── emissive ──
    surface.Emissive = gMaterial.Emissive.rgb;
#ifdef _EMISSIVE_MAP
    surface.Emissive *= gEmissiveMap.Sample(gSampler, texcoord).rgb;
#endif

    return surface;
}

/// 把采样结果与 cbuffer 里的 Principled 扩展参数组装成 BSDF 输入。
PrincipledMaterial make_standard_principled_material(SurfaceSample surface) {
    PrincipledMaterial material =
        make_principled_material(surface.Albedo, surface.Metallic, surface.Roughness);
    material.Specular = saturate(gMaterial.Principled0.x);
    material.SpecularTint = saturate(gMaterial.Principled0.y);
    material.Clearcoat = saturate(gMaterial.Principled0.z);
    material.ClearcoatGloss = saturate(gMaterial.Principled0.w);
    material.Sheen = saturate(gMaterial.Principled1.x);
    material.SheenTint = saturate(gMaterial.Principled1.y);
    material.Anisotropy = saturate(gMaterial.Principled1.z);
    material.Flatness = saturate(gMaterial.Principled1.w);
    material.SpecularTrans = saturate(gMaterial.Principled2.x);
    // eta 必须 > 1, 否则菲涅尔项退化。
    material.Eta = max(gMaterial.Principled2.y, 1.001f);
    return material;
}

#endif
