#ifndef RADRAY_SHADOW_CUBE_HLSLI
#define RADRAY_SHADOW_CUBE_HLSLI

#include <shadow/filtering.hlsli>

// 点光源立方体阴影 (omnidirectional cube shadow)。
//
// 参考 UE5 的 OnePassPointLightShadow, 但用引擎的标准深度约定 (clear = 1.0, 比较 Less)
// 而非 UE5 的 reverse-Z。资源是 TextureCube<float> 深度图: 采样时给方向向量, 硬件自动选面;
// 面矩阵只用来把世界坐标重投影成待比较的深度值。
//
// 数据流 (与 CPU 端点光源阴影数据对齐):
//   生成: 6 面各一次 render (或一次 layered draw), 逐面用 ViewProj[face] 写入对应 cube slice。
//   采样: 用 光->片元 方向做 SampleCmpLevelZero, 比较深度由 ViewProj[face] 重投影得到。

#define RADRAY_CUBE_FACE_COUNT 6

/// 一盏投阴影的点光源的完整阴影数据。列主序, 须与 CPU 端逐字段对齐。
struct CubeShadow {
    // 6 面的 世界 -> 裁剪 矩阵。面序为 (+X, -X, +Y, -Y, +Z, -Z),
    // 须与 CPU 端 6 面视图矩阵的生成顺序及 select_cube_face 严格一致。
    float4x4 ViewProj[RADRAY_CUBE_FACE_COUNT];
    float4 LightPositionInvRadius;  // xyz = 光源世界位置, w = 1 / 半径
    // x = depthBias (世界空间, 已按 texel 世界尺寸缩放)
    // y = normalBias (世界空间, 同上)
    // z = 1 / 阴影图边长 (预留给宽 PCF)
    // w = enable (>= 0.5 启用)
    float4 Params;
};

bool cube_shadow_enabled(CubeShadow shadow) {
    return shadow.Params.w >= 0.5f;
}

/// 方向向量投影到的 cube 面 (0..5 = +X, -X, +Y, -Y, +Z, -Z)。
/// 取绝对值最大的轴, 与 CPU 端 6 面矩阵的生成顺序严格对齐。
uint select_cube_face(float3 dir) {
    float3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z) {
        return dir.x > 0.0f ? 0u : 1u;
    }
    if (a.y >= a.z) {
        return dir.y > 0.0f ? 2u : 3u;
    }
    return dir.z > 0.0f ? 4u : 5u;
}

/// 采样点光源立方体阴影, 返回可见度 [0, 1] (1 = 完全受光, 0 = 完全遮蔽)。
///   shadow_cube:    cube 深度图
///   cmp:            比较采样器 (Less / LessEqual)
///   shadow:         该光源的阴影数据
///   position_world: 着色点世界坐标
///   normal_world:   着色点世界法线 (无法线时传 0 并把 normalBias 设 0)
float sample_cube_shadow(
    TextureCube<float> shadow_cube,
    SamplerComparisonState cmp,
    CubeShadow shadow,
    float3 position_world,
    float3 normal_world) {
    if (!cube_shadow_enabled(shadow)) {
        return 1.0f;
    }

    float3 to_light = shadow.LightPositionInvRadius.xyz - position_world;
    float dist = length(to_light);
    if (dist * shadow.LightPositionInvRadius.w >= 1.0f) {
        return 1.0f;  // 超出光照半径, 阴影图未覆盖
    }
    float3 dir_to_light = to_light * safe_rcp(dist);

    float3 biased = apply_shadow_bias(
        position_world, normal_world, dir_to_light, shadow.Params.x, shadow.Params.y);

    // 用 光->片元 方向选面, 再用该面矩阵重投影出待比较深度。
    uint face = select_cube_face(-to_light);
    float4 clip = mul(shadow.ViewProj[face], float4(biased, 1.0f));
    if (clip.w <= 0.0f) {
        return 1.0f;  // 落在该面的近裁剪面之后
    }

    // cube 比较采样: 方向为世界空间 光->片元, 硬件自动选面并做 2x2 PCF。
    return shadow_cube.SampleCmpLevelZero(cmp, -to_light, clip.z / clip.w);
}

#endif
