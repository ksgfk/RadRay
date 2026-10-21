#ifndef RADRAY_SHADOW_FILTERING_HLSLI
#define RADRAY_SHADOW_FILTERING_HLSLI

#include <core/math.hlsli>

// 各类阴影技术共用的采样原语: 世界->阴影投影、bias、PCF 核。
// 具体技术各自成文件并包含本文件:
//   shadow/cascade.hlsli — 方向光级联阴影 (Texture2DArray)
//   shadow/cube.hlsli    — 点光源立方体阴影 (TextureCube)
//
// 深度约定: 标准深度 (clear = 1.0, 越近越小, 比较函数 Less/LessEqual), 非 reverse-Z。

/// PCF 过滤质量。数值是 CPU 端序列化过的 ABI, 只能追加不能重排。
/// 见 render_framework/directional_light_scene_proxy.h 的 GetShadowSoftMode()。
#define RADRAY_SHADOW_FILTER_HARD 0u    // 单 tap, 硬边
#define RADRAY_SHADOW_FILTER_LOW 1u     // 4-tap 双线性
#define RADRAY_SHADOW_FILTER_MEDIUM 2u  // 5x5 tent, 9 次双线性 fetch

/// 阴影图尺寸的预打包形式: xy = 1/size (texel 尺寸), zw = size (像素数)。
/// 打包成一个 float4 是为了让 tent 核少算几次除法。
float4 make_shadowmap_size(float size) {
    float clamped = max(size, 1.0f);
    float inv = 1.0f / clamped;
    return float4(inv, inv, clamped, clamped);
}

/// 世界坐标 -> 阴影图 uv + 待比较深度。
/// inside 为 false 表示落在该阴影视锥外, 调用方应视为无遮蔽。
float3 world_to_shadow_uv(float4x4 world_to_shadow, float3 position_world, out bool inside) {
    float4 clip = mul(world_to_shadow, float4(position_world, 1.0f));
    float3 ndc = clip.xyz / clip.w;
    // NDC -> uv: x 直接映射, y 翻转 (纹理原点在左上)。
    float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    inside = (ndc.z > 0.0f && ndc.z < 1.0f && all(uv >= 0.0f) && all(uv <= 1.0f));
    return float3(uv, ndc.z);
}

/// 沿光线方向 + 沿法线偏移采样点, 抑制自阴影粉刺 (shadow acne)。
///
/// 两个 bias 都是【世界空间】长度, 调用方须已按该级联/该面的 texel 世界尺寸缩放过 ——
/// 这样不同分辨率与不同级联的表现才一致。normal bias 按掠射程度加权, 正对光源时不偏移。
float3 apply_shadow_bias(
    float3 position_world,
    float3 normal_world,
    float3 dir_to_light,
    float depth_bias,
    float normal_bias) {
    float grazing = 1.0f - saturate(dot(dir_to_light, normal_world));
    position_world += dir_to_light * depth_bias;
    position_world += normal_world * (grazing * normal_bias);
    return position_world;
}

// ── 5x5 tent 核的权重推导 ───────────────────────────────────────────────────
// 思路: 把 5x5 三角形滤波核按 texel 面积积分, 折成 9 次硬件双线性 fetch。
// 每次 fetch 的 uv 落在两两 texel 之间, 由硬件的双线性插值代替手工求 4 个 tap。
// 移植自 Unity URP 的 SampleShadow_ComputeSamples_Tent_5x5, 数值行为保持一致。

/// 单个三角形覆盖的 texel 面积。
float shadow_tent_triangle_area(float height) {
    return height - 0.5f;
}

/// 3x3 tent 在一个轴上的 texel 面积。computed_area 为裁剪后, uncut 为未裁剪。
void shadow_tent_texel_areas_3x3(float offset, out float4 computed_area, out float4 computed_area_uncut) {
    float offset01_squared_halved = (offset + 0.5f) * (offset + 0.5f) * 0.5f;
    computed_area_uncut.x = computed_area.x = offset01_squared_halved - offset;
    computed_area_uncut.w = computed_area.w = offset01_squared_halved;

    computed_area_uncut.y = shadow_tent_triangle_area(1.5f - offset);
    float clamped_left = min(offset, 0.0f);
    computed_area.y = computed_area_uncut.y - clamped_left * clamped_left;

    computed_area_uncut.z = shadow_tent_triangle_area(1.5f + offset);
    float clamped_right = max(offset, 0.0f);
    computed_area.z = computed_area_uncut.z - clamped_right * clamped_right;
}

/// 5x5 tent 在一个轴上的 6 个 texel 权重 (前 3 个 + 后 3 个)。0.16 = 1/6.25 的归一化系数。
void shadow_tent_texel_weights_5x5(float offset, out float3 weights_a, out float3 weights_b) {
    float4 computed_area;
    float4 computed_area_uncut;
    shadow_tent_texel_areas_3x3(offset, computed_area, computed_area_uncut);

    weights_a.x = 0.16f * computed_area.x;
    weights_a.y = 0.16f * computed_area_uncut.y;
    weights_a.z = 0.16f * (computed_area.y + 1.0f);
    weights_b.x = 0.16f * (computed_area.z + 1.0f);
    weights_b.y = 0.16f * computed_area_uncut.z;
    weights_b.z = 0.16f * computed_area.w;
}

/// 把 5x5 tent 核折成 9 组 (权重, uv)。
void shadow_tent_samples_5x5(
    float4 shadowmap_size,
    float2 uv,
    out float weights[9],
    out float2 sample_uv[9]) {
    float2 tent_center_texel = uv * shadowmap_size.zw;
    float2 fetch_center_texel = floor(tent_center_texel + 0.5f);
    float2 offset = tent_center_texel - fetch_center_texel;

    float3 weights_u_a, weights_u_b;
    float3 weights_v_a, weights_v_b;
    shadow_tent_texel_weights_5x5(offset.x, weights_u_a, weights_u_b);
    shadow_tent_texel_weights_5x5(offset.y, weights_v_a, weights_v_b);

    // 相邻两个 texel 的权重合并成一次双线性 fetch。
    float3 fetch_weights_u = float3(weights_u_a.xz, weights_u_b.y) + float3(weights_u_a.y, weights_u_b.xz);
    float3 fetch_weights_v = float3(weights_v_a.xz, weights_v_b.y) + float3(weights_v_a.y, weights_v_b.xz);

    // 合并后的采样位置按权重比例落在两 texel 之间, 让硬件插值出正确的加权和。
    float3 fetch_offsets_u = float3(weights_u_a.y, weights_u_b.xz) / fetch_weights_u + float3(-2.5f, -0.5f, 1.5f);
    float3 fetch_offsets_v = float3(weights_v_a.y, weights_v_b.xz) / fetch_weights_v + float3(-2.5f, -0.5f, 1.5f);
    fetch_offsets_u *= shadowmap_size.xxx;
    fetch_offsets_v *= shadowmap_size.yyy;

    float2 origin = fetch_center_texel * shadowmap_size.xy;
    sample_uv[0] = origin + float2(fetch_offsets_u.x, fetch_offsets_v.x);
    sample_uv[1] = origin + float2(fetch_offsets_u.y, fetch_offsets_v.x);
    sample_uv[2] = origin + float2(fetch_offsets_u.z, fetch_offsets_v.x);
    sample_uv[3] = origin + float2(fetch_offsets_u.x, fetch_offsets_v.y);
    sample_uv[4] = origin + float2(fetch_offsets_u.y, fetch_offsets_v.y);
    sample_uv[5] = origin + float2(fetch_offsets_u.z, fetch_offsets_v.y);
    sample_uv[6] = origin + float2(fetch_offsets_u.x, fetch_offsets_v.z);
    sample_uv[7] = origin + float2(fetch_offsets_u.y, fetch_offsets_v.z);
    sample_uv[8] = origin + float2(fetch_offsets_u.z, fetch_offsets_v.z);

    weights[0] = fetch_weights_u.x * fetch_weights_v.x;
    weights[1] = fetch_weights_u.y * fetch_weights_v.x;
    weights[2] = fetch_weights_u.z * fetch_weights_v.x;
    weights[3] = fetch_weights_u.x * fetch_weights_v.y;
    weights[4] = fetch_weights_u.y * fetch_weights_v.y;
    weights[5] = fetch_weights_u.z * fetch_weights_v.y;
    weights[6] = fetch_weights_u.x * fetch_weights_v.z;
    weights[7] = fetch_weights_u.y * fetch_weights_v.z;
    weights[8] = fetch_weights_u.z * fetch_weights_v.z;
}

// ── Texture2DArray 上的比较采样 ─────────────────────────────────────────────
// 全部返回可见度 [0, 1]: 1 = 完全受光, 0 = 完全遮蔽。

/// 单 tap 硬件比较采样 (硬件自带 2x2 PCF)。
float sample_shadow_array_hard(
    Texture2DArray<float> shadow_map,
    SamplerComparisonState cmp,
    float2 uv,
    float slice,
    float depth) {
    return shadow_map.SampleCmpLevelZero(cmp, float3(uv, slice), depth);
}

/// 4-tap: 半 texel 偏移的四角平均。
float sample_shadow_array_low(
    Texture2DArray<float> shadow_map,
    SamplerComparisonState cmp,
    float4 shadowmap_size,
    float2 uv,
    float slice,
    float depth) {
    float2 h = shadowmap_size.xy * 0.5f;
    float4 taps;
    taps.x = sample_shadow_array_hard(shadow_map, cmp, uv + float2(-h.x, -h.y), slice, depth);
    taps.y = sample_shadow_array_hard(shadow_map, cmp, uv + float2(h.x, -h.y), slice, depth);
    taps.z = sample_shadow_array_hard(shadow_map, cmp, uv + float2(-h.x, h.y), slice, depth);
    taps.w = sample_shadow_array_hard(shadow_map, cmp, uv + float2(h.x, h.y), slice, depth);
    return dot(taps, (float4)0.25f);
}

/// 5x5 tent: 9 次加权双线性 fetch。
float sample_shadow_array_medium(
    Texture2DArray<float> shadow_map,
    SamplerComparisonState cmp,
    float4 shadowmap_size,
    float2 uv,
    float slice,
    float depth) {
    float weights[9];
    float2 sample_uv[9];
    shadow_tent_samples_5x5(shadowmap_size, uv, weights, sample_uv);

    float visibility = 0.0f;
    [unroll]
    for (int i = 0; i < 9; ++i) {
        visibility += weights[i] * sample_shadow_array_hard(shadow_map, cmp, sample_uv[i], slice, depth);
    }
    return visibility;
}

/// 按 RADRAY_SHADOW_FILTER_* 分派。filter 来自 cbuffer, 故这里是运行期分支。
float sample_shadow_array(
    Texture2DArray<float> shadow_map,
    SamplerComparisonState cmp,
    float4 shadowmap_size,
    float2 uv,
    float slice,
    float depth,
    uint filter) {
    if (filter == RADRAY_SHADOW_FILTER_LOW) {
        return sample_shadow_array_low(shadow_map, cmp, shadowmap_size, uv, slice, depth);
    }
    if (filter == RADRAY_SHADOW_FILTER_MEDIUM) {
        return sample_shadow_array_medium(shadow_map, cmp, shadowmap_size, uv, slice, depth);
    }
    return sample_shadow_array_hard(shadow_map, cmp, uv, slice, depth);
}

#endif
