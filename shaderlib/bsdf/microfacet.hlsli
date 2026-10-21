#ifndef RADRAY_BSDF_MICROFACET_HLSLI
#define RADRAY_BSDF_MICROFACET_HLSLI

#include <core/math.hlsli>

// 微表面分布 (D) 与遮蔽阴影 (G) 项。全部在着色局部系 (n = +Z) 求值。
//
// 命名: D_* 为法线分布, G_* 为双向遮蔽, lambda_* / G1_* 为单向。
// 各向异性参数一律为 (alpha_x, alpha_y) 而非 roughness —— 转换在 roughness_to_alpha 里做一次。

/// roughness -> GGX alpha。
///
/// has_anisotropy 为 false 时返回各向同性的 (a, a)。anisotropy 在 [0, 1) 内取值,
/// 越大则切线方向越被拉长。下限 1e-3 防止 D 项在完美镜面处发散。
float2 roughness_to_alpha(float roughness, float anisotropy, bool has_anisotropy) {
    float a = roughness * roughness;
    if (!has_anisotropy) {
        float iso = max(a, 1e-3f);
        return float2(iso, iso);
    }
    float aspect = sqrt(max(1.0f - 0.9f * anisotropy, 1e-3f));
    return float2(max(a / aspect, 1e-3f), max(a * aspect, 1e-3f));
}

/// 各向异性 GGX (Trowbridge-Reitz) 法线分布。m 为微表面法线 (半程向量)。
float D_GGX(float3 m, float alpha_x, float alpha_y) {
    if (m.z <= 0.0f) {
        return 0.0f;
    }
    float sx = m.x / alpha_x;
    float sy = m.y / alpha_y;
    float denom = sx * sx + sy * sy + m.z * m.z;
    return max(1.0f / (RADRAY_PI * alpha_x * alpha_y * denom * denom), 0.0f);
}

/// 各向异性 GGX 的 Smith 单向遮蔽项。
float G1_SmithGGX(float3 v, float alpha_x, float alpha_y) {
    if (v.z <= 0.0f) {
        return 0.0f;
    }
    float vx = v.x * alpha_x;
    float vy = v.y * alpha_y;
    float tan2 = (vx * vx + vy * vy) / (v.z * v.z);
    float lambda = 0.5f * (sqrt(1.0f + tan2) - 1.0f);
    return 1.0f / (1.0f + lambda);
}

/// 分离式 Smith 双向遮蔽 (separable form)。
float G_SmithGGX(float3 wi, float3 wo, float alpha_x, float alpha_y) {
    return G1_SmithGGX(wi, alpha_x, alpha_y) * G1_SmithGGX(wo, alpha_x, alpha_y);
}

/// GTR1 (Berry) 分布, Disney 用它做清漆层的长尾高光。各向同性。
float D_GTR1(float3 m, float alpha) {
    float cos_theta = m.z;
    if (cos_theta <= 0.0f) {
        return 0.0f;
    }
    float alpha2 = alpha * alpha;
    float denom = RADRAY_PI * log(alpha2) * (1.0f + (alpha2 - 1.0f) * cos_theta * cos_theta);
    float d = (alpha2 - 1.0f) / denom;
    return (d * cos_theta > 1e-20f) ? d : 0.0f;
}

/// 清漆层的 Smith 单向遮蔽。Disney 固定用 alpha = 0.25 求 G, 与 D 用的 alpha 无关。
float G1_SmithGGX_Clearcoat(float3 v, float3 m, float alpha) {
    float cos_theta = abs(v.z);
    float cos2 = cos_theta * cos_theta;
    if (cos2 <= 0.0f) {
        return 0.0f;
    }
    // 与 m 异侧的方向不参与该微表面的能量传输。
    if (dot(v, m) * v.z <= 0.0f) {
        return 0.0f;
    }
    if (v.z == 1.0f) {
        return 1.0f;
    }
    float tan2 = (1.0f - cos2) / cos2;
    return 2.0f * rcp(1.0f + sqrt(1.0f + alpha * alpha * tan2));
}

/// 清漆层双向遮蔽。
float G_SmithGGX_Clearcoat(float3 wi, float3 wo, float3 m, float alpha) {
    return G1_SmithGGX_Clearcoat(wi, m, alpha) * G1_SmithGGX_Clearcoat(wo, m, alpha);
}

#endif
