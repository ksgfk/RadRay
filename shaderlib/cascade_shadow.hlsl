#ifndef RADRAY_CASCADE_SHADOW_HLSL
#define RADRAY_CASCADE_SHADOW_HLSL

// 方向光级联阴影 (Cascaded Shadow Maps, CSM)。
//
// 参考 UE5 的 CSM (practical split scheme + 稳定化 texel snapping), 但采用引擎现有的
// 标准深度约定 (clear=1.0, 比较 LessEqual), 而非 UE5 的 reverse-Z。资源为
// Texture2DArray<float> 深度图, 每级联一层; 逐级联用其 世界->阴影裁剪 矩阵重投影出待
// 比较的深度, 用比较采样器做 PCF。
//
// 数据流 (与 CPU 端 ForwardPipeline::CascadeShadowGpu 对齐):
//   生成阶段: 逐级联一次 render, 用 WorldToShadow[i] 把世界坐标投影进第 i 层深度。
//   采样阶段: 先按包围球选级联, 再用该级联矩阵重投影得 uvz, 逐级联层比较采样。
//
// 结构为定长 (RADRAY_MAX_CASCADES 个矩阵), 可整块塞进普通 cbuffer, 无需 StructuredBuffer。
//
// PCF / 世界->阴影 投影 / 偏移 等通用采样设施复用 shadow.hlsl (与 spot 等其它阴影共享)。

#include "shadow.hlsl"

#define RADRAY_MAX_CASCADES 4

// 一盏投影级联阴影的方向光的完整阴影数据 (匹配 CPU 端 CascadeShadowGpu, 列主序)。
struct ShadowParam {
    float4x4 WorldToShadow[RADRAY_MAX_CASCADES];  // 逐级联 世界 -> 阴影裁剪
    float4 CascadeSphere[RADRAY_MAX_CASCADES];    // xyz=中心, w=半径^2 (供选级联)
    // 逐级联 world-space bias: xy = (depthBias, normalBias) 已按该级联的 texel 世界尺寸缩放,
    // zw 保留。CPU 端由用户无量纲倍率 (0..N) 乘以逐级联 texelSize 得到, 故不同级联/分辨率自动一致。
    float4 CascadeBias[RADRAY_MAX_CASCADES];
    // x = enable (>= 0.5 启用, < 0.5 直接返回无阴影)
    // y = shadowmap size (px, 供 PCF 计算 texel 尺寸)
    // z = cascade count (实际级联数)
    // w = soft mode (0 = 单 tap, 1 = 4-tap, 2 = 5x5 tent)
    float4 Params;
};

// 选择着色点所落的级联 (取第一个包围球命中的级联)。未命中返回 count。
uint compute_cascade_index(ShadowParam sp, float3 posW, uint count) {
    uint index = count;
    [unroll]
    for (uint i = 0; i < RADRAY_MAX_CASCADES; ++i) {
        if (i >= count) {
            break;
        }
        float3 d = posW - sp.CascadeSphere[i].xyz;
        float dist2 = dot(d, d);
        if (dist2 < sp.CascadeSphere[i].w && index == count) {
            index = i;
        }
    }
    return index;
}

// 采样方向光级联阴影, 返回可见度 [0,1] (1 = 完全受光, 0 = 完全遮蔽)。
//   shadowMap:  级联深度图 (Texture2DArray, 标准深度, 越近值越小)
//   cmp:        比较采样器 (CompareFunction::Less / LessEqual)
//   sp:         该方向光的级联阴影数据
//   posW:       着色点世界坐标 (原始, 未加 bias)
//   normalW:    着色点世界法线 (归一化; 用于 normal bias)
//   dirToLight: 指向光源的方向 (归一化; 用于 depth/normal bias)
//
// 先按包围球选级联, 再用该级联的 world-space bias 偏移采样点 (抗自阴影粉刺), 最后逐级联比较采样。
float sample_shadow_cascade(
    Texture2DArray<float> shadowMap,
    SamplerComparisonState cmp,
    ShadowParam sp,
    float3 posW,
    float3 normalW,
    float3 dirToLight) {
    if (sp.Params.x < 0.5f) {
        return 1.0f;
    }

    uint count = (uint)sp.Params.z;
    uint cascadeIndex = compute_cascade_index(sp, posW, count);
    if (cascadeIndex >= count) {
        return 1.0f;
    }

    // 逐级联 world-space bias: 沿光线方向 + 沿法线偏移采样点, 再重投影。
    float2 bias = sp.CascadeBias[cascadeIndex].xy;
    float3 biasedPosW = apply_shadow_bias(posW, normalW, dirToLight, bias.x, bias.y);

    bool inside;
    float3 uvz = world_to_shadow_uv(sp.WorldToShadow[cascadeIndex], biasedPosW, inside);
    if (!inside) {
        return 1.0f;
    }

    float slice = (float)cascadeIndex;
    float size = max(sp.Params.y, 1.0f);
    float4 shadowmapSize = float4(1.0f / size, 1.0f / size, size, size);

    uint softMode = (uint)sp.Params.w;
    if (softMode == 1u) {
        return sample_shadow_low(shadowMap, cmp, shadowmapSize, uvz.xy, slice, uvz.z);
    }
    if (softMode == 2u) {
        return sample_shadow_medium(shadowMap, cmp, shadowmapSize, uvz.xy, slice, uvz.z);
    }
    return sample_shadow_tap(shadowMap, cmp, uvz.xy, slice, uvz.z);
}

#endif
