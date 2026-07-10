#ifndef RADRAY_POINT_SHADOW_HLSL
#define RADRAY_POINT_SHADOW_HLSL

// 点光源立方体阴影 (point light cube shadow)。
//
// 参考 UE5 的 OnePassPointLightShadow, 但采用引擎现有的标准深度约定 (clear=1.0, 比较 Less),
// 而非 UE5 的 reverse-Z。资源为 TextureCube<float> 深度图, 用方向向量做硬件比较采样,
// 硬件自动选面; 面矩阵仅用于把世界坐标重投影出待比较的深度值。
//
// 数据流 (与 CPU 端 PointShadowData 对齐):
//   生成阶段: 6 面各一次 render, 逐面用 ViewProj[face] 把世界坐标投影进对应 cube slice 的深度。
//   采样阶段: 用 -toLight 方向 SampleCmpLevelZero, compareDepth 由 ViewProj[face] 重投影得到。
//
// 结构为定长 (6 面矩阵), 可整块塞进普通 cbuffer, 无需 StructuredBuffer。

#include "common.hlsl"
#include "shadow.hlsl"

// 一个投影阴影的点光源的完整阴影数据 (匹配 CPU 端 PointShadowData, 列主序)。
struct PointShadowData {
    // 6 个面的 世界 -> 裁剪 矩阵。面序 = cube_face_id 的 (+X,-X,+Y,-Y,+Z,-Z),
    // 与 CPU 端 6 面视图矩阵生成顺序严格对齐。
    float4x4 ViewProj[6];
    float4 LightPosInvRadius;  // xyz 光源世界位置, w = 1 / radius
    // x = depthBias (沿光线方向的世界空间偏移, 已按 texel 世界尺寸缩放)
    // y = normalBias (沿法线的世界空间偏移, 已按 texel 世界尺寸缩放; 抗自阴影粉刺)
    // z = invResolution (1 / 阴影图边长像素数, 预留给宽 PCF)
    // w = enable (>= 0.5 启用, < 0.5 直接返回无阴影)
    float4 Params;
};

// 选择方向向量投影到的 cube 面 (0..5 = +X,-X,+Y,-Y,+Z,-Z)。
// 与 CPU 端 6 面矩阵的生成顺序严格对齐。
uint point_shadow_cube_face(float3 dir) {
    float3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z) {
        return dir.x > 0.0f ? 0u : 1u;
    }
    if (a.y >= a.z) {
        return dir.y > 0.0f ? 2u : 3u;
    }
    return dir.z > 0.0f ? 4u : 5u;
}

// 采样点光源立方体阴影, 返回可见度 [0,1] (1 = 完全受光, 0 = 完全遮蔽)。
//   shadowCube:  cube 深度图 (标准深度, 越近值越小)
//   cmp:         比较采样器 (CompareFunction::Less / LessEqual)
//   sd:          该光源的阴影数据
//   posW:        着色点世界坐标
//   normalW:     着色点世界法线 (用于 bias; 无法线可传 0 并把 normalBias 设 0)
float sample_point_shadow(
    TextureCube<float> shadowCube,
    SamplerComparisonState cmp,
    PointShadowData sd,
    float3 posW,
    float3 normalW) {
    if (sd.Params.w < 0.5f) {
        return 1.0f;  // 该光源未启用阴影
    }

    float3 toLight = sd.LightPosInvRadius.xyz - posW;
    float dist = length(toLight);
    if (dist * sd.LightPosInvRadius.w >= 1.0f) {
        return 1.0f;  // 超出光照半径, 不在阴影范围内
    }
    float3 dirToLight = toLight / max(dist, 1e-6f);

    // world-space bias: 沿光线方向 + 沿法线偏移采样点 (与级联阴影一致), 再重投影。
    float3 biasedPosW = apply_shadow_bias(posW, normalW, dirToLight, sd.Params.x, sd.Params.y);

    // 用光->片元方向选面, 取该面矩阵把世界坐标重投影, 得到待比较深度。
    uint face = point_shadow_cube_face(-toLight);
    float4 clip = mul(sd.ViewProj[face], float4(biasedPosW, 1.0f));
    if (clip.w <= 0.0f) {
        return 1.0f;
    }
    float compareDepth = clip.z / clip.w;

    // 硬件比较采样: cube 采样方向为 世界空间 光->片元 向量, 硬件自动选面 + 2x2 PCF。
    return shadowCube.SampleCmpLevelZero(cmp, -toLight, compareDepth);
}

#endif
