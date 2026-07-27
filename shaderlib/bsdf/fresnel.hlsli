#ifndef RADRAY_BSDF_FRESNEL_HLSLI
#define RADRAY_BSDF_FRESNEL_HLSLI

#include <core/math.hlsli>

// 菲涅尔项。全部约定在着色局部系 (n = +Z), 故 cos_theta 即方向向量的 z 分量。
//
// cos_theta_i 的符号带信息: > 0 表示方向在法线正侧 (外部入射), < 0 表示背侧 (内部入射)。
// 因此 eta 的取用方向由符号决定, 调用方不要预先取绝对值。

/// 介电体菲涅尔折射比的完整解 (未偏振, s/p 平均)。
///
/// eta = 透射侧 IOR / 入射侧 IOR (相对折射率)。
/// 除反射率外还输出折射几何, 供 BTDF 与 Schlick 近似复用:
///   cos_theta_t: 折射角余弦, 符号与 cos_theta_i 相反; 全内反射时为 0
///   eta_it / eta_ti: 按入射侧修正后的相对折射率及其倒数
float fresnel_dielectric(
    float cos_theta_i,
    float eta,
    out float cos_theta_t,
    out float eta_it,
    out float eta_ti) {
    bool outside = cos_theta_i >= 0.0f;
    float rcp_eta = 1.0f / eta;
    eta_it = outside ? eta : rcp_eta;
    eta_ti = outside ? rcp_eta : eta;

    float cos_i = abs(cos_theta_i);
    // Snell: sin_t^2 = (sin_i * eta_ti)^2, 负值即全内反射。
    float cos_t_sqr = 1.0f - (1.0f - cos_i * cos_i) * (eta_ti * eta_ti);
    float cos_t = sqrt(max(cos_t_sqr, 0.0f));

    float a_s = (eta_it * cos_i - cos_t) / (eta_it * cos_i + cos_t);
    float a_p = (eta_it * cos_t - cos_i) / (eta_it * cos_t + cos_i);
    float r = 0.5f * (a_s * a_s + a_p * a_p);

    // eta == 1 (无界面) 与掠射 (cos_i == 0) 会让上面的比值退化成 0/0。
    bool index_matched = (eta == 1.0f);
    bool degenerate = index_matched || (cos_i == 0.0f);
    r = degenerate ? (index_matched ? 0.0f : 1.0f) : r;

    cos_theta_t = (cos_t_sqr >= 0.0f) ? (-sign(cos_theta_i) * cos_t) : 0.0f;
    return r;
}

/// Schlick 的 (1 - cos)^5 权重项。
float fresnel_schlick_weight(float cos_theta) {
    return pow5(saturate(1.0f - cos_theta));
}

/// 由相对折射率反推垂直入射反射率 R0。
float fresnel_schlick_r0(float eta) {
    return pow2((eta - 1.0f) / (eta + 1.0f));
}

/// 介电体的 Schlick 权重, 自动处理内部入射。
/// 从密介质射向疏介质时 (eta_it <= 1) 必须用折射角, 否则全内反射区的能量会算错。
float fresnel_schlick_weight_dielectric(float cos_theta_i, float eta) {
    float cos_theta_t;
    float eta_it;
    float eta_ti;
    fresnel_dielectric(cos_theta_i, eta, cos_theta_t, eta_it, eta_ti);
    return (eta_it > 1.0f)
               ? fresnel_schlick_weight(abs(cos_theta_i))
               : fresnel_schlick_weight(abs(cos_theta_t));
}

/// R0 到 1 之间按 Schlick 权重插值 (标量版)。
float fresnel_schlick(float r0, float cos_theta_i, float eta) {
    return lerp(fresnel_schlick_weight_dielectric(cos_theta_i, eta), 1.0f, r0);
}

/// R0 到 1 之间按 Schlick 权重插值 (有色 R0 版)。
float3 fresnel_schlick(float3 r0, float cos_theta_i, float eta) {
    return lerp((float3)fresnel_schlick_weight_dielectric(cos_theta_i, eta), (float3)1.0f, r0);
}

#endif
