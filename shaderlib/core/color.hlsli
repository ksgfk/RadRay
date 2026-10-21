#ifndef RADRAY_CORE_COLOR_HLSLI
#define RADRAY_CORE_COLOR_HLSLI

#include <core/math.hlsli>

// 色彩空间变换与 tone mapping。只做"数值 -> 数值"的映射, 不涉及光照模型。

/// Rec.709 / sRGB 原色下的相对亮度。
float luminance(float3 c) {
    return dot(c, float3(0.212671f, 0.715160f, 0.072169f));
}

/// 线性 -> sRGB (IEC 61966-2-1 精确曲线, 含低端线性段)。
/// 只在最终写 UNORM 且 target 非 _SRGB 格式时调用; target 已是 _SRGB 时硬件会再做一次,
/// 重复编码会让画面发灰。
float3 linear_to_srgb(float3 c) {
    float3 lo = c * 12.92f;
    float3 hi = 1.055f * pow(max(c, 0.0f), 1.0f / 2.4f) - 0.055f;
    return float3(
        c.x < 0.0031308f ? lo.x : hi.x,
        c.y < 0.0031308f ? lo.y : hi.y,
        c.z < 0.0031308f ? lo.z : hi.z);
}

/// sRGB -> 线性。手动解码非 _SRGB 格式里存的 sRGB 数据时用。
float3 srgb_to_linear(float3 c) {
    float3 lo = c / 12.92f;
    float3 hi = pow(max((c + 0.055f) / 1.055f, 0.0f), 2.4f);
    return float3(
        c.x <= 0.04045f ? lo.x : hi.x,
        c.y <= 0.04045f ? lo.y : hi.y,
        c.z <= 0.04045f ? lo.z : hi.z);
}

/// Reinhard tone map (逐通道)。会让饱和高光偏色, 只作为默认占位。
float3 tonemap_reinhard(float3 c) {
    return c / (c + 1.0f);
}

/// 按亮度做 Reinhard, 保留色相。比逐通道版本更适合有色高光。
float3 tonemap_reinhard_luminance(float3 c) {
    float l = luminance(c);
    return c * (safe_rcp(1.0f + l));
}

#endif
