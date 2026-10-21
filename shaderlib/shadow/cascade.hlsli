#ifndef RADRAY_SHADOW_CASCADE_HLSLI
#define RADRAY_SHADOW_CASCADE_HLSLI

#include <shadow/filtering.hlsli>

// 方向光级联阴影 (Cascaded Shadow Maps)。
//
// 参考 UE5 的 CSM (practical split + texel snapping 稳定化), 但用引擎的标准深度约定
// (clear = 1.0, 比较 LessEqual) 而非 UE5 的 reverse-Z。
//
// 数据流 (与 CPU 端 ForwardPipeline 的级联阴影数据对齐):
//   生成: 逐级联一次 render, 用 WorldToShadow[i] 把世界坐标投影进 Texture2DArray 第 i 层。
//   采样: 先按包围球选级联, 再用该级联矩阵重投影得 uvz, 在对应层上比较采样。
//
// 定长结构 (RADRAY_MAX_CASCADES 个矩阵), 可整块塞进普通 cbuffer, 不需要 StructuredBuffer。

#define RADRAY_MAX_CASCADES 4

/// 一盏投级联阴影的方向光的完整阴影数据。列主序, 须与 CPU 端逐字段对齐。
struct CascadeShadow {
    float4x4 WorldToShadow[RADRAY_MAX_CASCADES];  // 逐级联 世界 -> 阴影裁剪空间
    float4 CascadeSphere[RADRAY_MAX_CASCADES];    // xyz = 包围球中心, w = 半径^2
    // 逐级联世界空间 bias: xy = (depthBias, normalBias), zw 保留。
    // CPU 端已把用户给的无量纲倍率乘上该级联的 texel 世界尺寸, 故各级联/各分辨率表现一致。
    float4 CascadeBias[RADRAY_MAX_CASCADES];
    // x = enable (>= 0.5 启用)
    // y = 阴影图边长 (px)
    // z = 实际级联数
    // w = RADRAY_SHADOW_FILTER_* 过滤模式
    float4 Params;
};

bool cascade_shadow_enabled(CascadeShadow shadow) {
    return shadow.Params.x >= 0.5f;
}

/// 选着色点所属的级联: 取第一个命中包围球的。未命中返回 count。
uint select_cascade_index(CascadeShadow shadow, float3 position_world, uint count) {
    uint index = count;
    [unroll]
    for (uint i = 0; i < RADRAY_MAX_CASCADES; ++i) {
        if (i >= count) {
            break;
        }
        float3 d = position_world - shadow.CascadeSphere[i].xyz;
        if (dot(d, d) < shadow.CascadeSphere[i].w && index == count) {
            index = i;
        }
    }
    return index;
}

/// 采样方向光级联阴影, 返回可见度 [0, 1] (1 = 完全受光, 0 = 完全遮蔽)。
///   shadow_map:   级联深度图, 每级联一层
///   cmp:          比较采样器 (Less / LessEqual)
///   shadow:       该方向光的级联阴影数据
///   position_world: 着色点世界坐标 (原始, 未加 bias)
///   normal_world:   着色点世界法线 (归一化)
///   dir_to_light:   指向光源的归一化方向
float sample_cascade_shadow(
    Texture2DArray<float> shadow_map,
    SamplerComparisonState cmp,
    CascadeShadow shadow,
    float3 position_world,
    float3 normal_world,
    float3 dir_to_light) {
    if (!cascade_shadow_enabled(shadow)) {
        return 1.0f;
    }

    uint count = (uint)shadow.Params.z;
    uint cascade = select_cascade_index(shadow, position_world, count);
    if (cascade >= count) {
        return 1.0f;  // 超出最远级联覆盖范围
    }

    float2 bias = shadow.CascadeBias[cascade].xy;
    float3 biased = apply_shadow_bias(position_world, normal_world, dir_to_light, bias.x, bias.y);

    bool inside;
    float3 uvz = world_to_shadow_uv(shadow.WorldToShadow[cascade], biased, inside);
    if (!inside) {
        return 1.0f;
    }

    return sample_shadow_array(
        shadow_map,
        cmp,
        make_shadowmap_size(shadow.Params.y),
        uvz.xy,
        (float)cascade,
        uvz.z,
        (uint)shadow.Params.w);
}

#endif
