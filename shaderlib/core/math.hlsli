#ifndef RADRAY_CORE_MATH_HLSLI
#define RADRAY_CORE_MATH_HLSLI

// 与渲染无关的标量数学常量与小工具。任何依赖光照/材质语义的东西都不属于这里。

static const float RADRAY_PI = 3.14159265358979323846f;
static const float RADRAY_INV_PI = 0.31830988618379067154f;
static const float RADRAY_TWO_PI = 6.28318530717958647692f;

// 除零保护用的统一下限。散落的 1e-6f 字面量一律走这个常量, 便于整体调参。
static const float RADRAY_EPS = 1e-6f;

float pow2(float x) {
    return x * x;
}

float pow4(float x) {
    return pow2(x) * pow2(x);
}

float pow5(float x) {
    return pow4(x) * x;
}

/// 安全倒数: 分母被抬到 RADRAY_EPS 以上, 避免 inf / NaN 顺着着色链条扩散。
float safe_rcp(float x) {
    return 1.0f / max(x, RADRAY_EPS);
}

/// 安全归一化: 零长度向量返回 fallback 而不是 NaN。
float3 safe_normalize(float3 v, float3 fallback) {
    float len2 = dot(v, v);
    return (len2 > RADRAY_EPS * RADRAY_EPS) ? (v * rsqrt(len2)) : fallback;
}

#endif
