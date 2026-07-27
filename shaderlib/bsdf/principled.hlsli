#ifndef RADRAY_BSDF_PRINCIPLED_HLSLI
#define RADRAY_BSDF_PRINCIPLED_HLSLI

#include <bsdf/fresnel.hlsli>
#include <bsdf/microfacet.hlsli>
#include <core/color.hlsli>

// Disney Principled BSDF 的反射半球部分, 与 Mitsuba3 src/bsdfs/principled.cpp 的 eval() 逐项对齐。
//
// ┌─ 方向约定 (最容易搞错的地方) ────────────────────────────────────────────┐
// │ 两个方向都在着色局部系 (n = +Z), 且沿用 Mitsuba 而非图形学常见的记法:      │
// │   wi = 视线方向, 指向【相机】 (= Mitsuba si.wi)                          │
// │   wo = 光照方向, 指向【光源】 (= Mitsuba eval 的第二参)                   │
// │ 这与"wi 指向光源"的习惯正好相反。                                        │
// │                                                                        │
// │ 返回值【已含出射侧余弦投影】:                                             │
// │   漫反射/清漆/绢光项乘了 abs(cos_theta_o), 即 N·L                        │
// │   镜面项只除 abs(cos_theta_i) —— 1/(4 cos_i cos_o) 里的 cos_o 被约掉      │
// │ 所以调用方【不得】再乘 N·L。调换 wi/wo 会同时弄错高光归一化与终止线。       │
// └────────────────────────────────────────────────────────────────────────┘

/// Principled 材质参数。取代原先 14 个位置参数, 避免调用方顺序写错。
/// 全部为已 saturate / 已 clamp 的最终值, 求值函数不再做范围修正。
struct PrincipledMaterial {
    float3 BaseColor;       // 线性空间基础色 (漫反射反照率 + 金属镜面色)
    float Metallic;         // 0 = 介电体, 1 = 导体
    float Roughness;        // 感知粗糙度, 内部平方成 GGX alpha
    float Specular;         // 介电体高光强度 (保留: 与 Eta 二选一, 当前由 Eta 主导)
    float SpecularTint;     // 高光向 BaseColor 色调偏移的比例
    float Anisotropy;       // 0 = 各向同性, 越大切线方向越被拉长
    float Sheen;            // 绢光强度 (布料边缘的漫反射高光)
    float SheenTint;        // 绢光向 BaseColor 色调偏移的比例
    float Flatness;         // 次表面近似的混合比例 (0 = 纯漫反射)
    float Clearcoat;        // 清漆层强度
    float ClearcoatGloss;   // 清漆层光泽度 (1 = 最锐利)
    float SpecularTrans;    // 镜面透射比例 (本函数只算反射, 它用于能量分配)
    float Eta;              // 相对折射率, 须 > 1
};

/// 各通道均为中性的默认材质。构造后按需覆盖字段, 免得漏初始化。
PrincipledMaterial make_principled_material(float3 base_color, float metallic, float roughness) {
    PrincipledMaterial m;
    m.BaseColor = base_color;
    m.Metallic = metallic;
    m.Roughness = roughness;
    m.Specular = 0.5f;
    m.SpecularTint = 0.0f;
    m.Anisotropy = 0.0f;
    m.Sheen = 0.0f;
    m.SheenTint = 0.0f;
    m.Flatness = 0.0f;
    m.Clearcoat = 0.0f;
    m.ClearcoatGloss = 0.0f;
    m.SpecularTrans = 0.0f;
    m.Eta = 1.5f;
    return m;
}

/// 主镜面瓣的菲涅尔项: 介电体基底 + 金属/高光染色的 Schlick 叠加。
///
/// f_dielectric 为该半程向量上的介电体菲涅尔值; front_side 为 false 时退化为纯透射权重。
float3 principled_fresnel(
    PrincipledMaterial m,
    float f_dielectric,
    float cos_theta_i,
    float luminance_base,
    float bsdf_weight,
    bool front_side) {
    float cos_theta_t;
    float eta_it;
    float eta_ti;
    fresnel_dielectric(cos_theta_i, m.Eta, cos_theta_t, eta_it, eta_ti);

    float3 f_schlick = (float3)0.0f;
    if (m.Metallic > 0.0f) {
        // 金属: 基础色直接当 R0。
        f_schlick += m.Metallic * fresnel_schlick(m.BaseColor, cos_theta_i, m.Eta);
    }
    if (m.SpecularTint > 0.0f) {
        // 高光染色: 保持亮度不变, 只借用基础色的色相。
        float3 tint = (luminance_base > 0.0f) ? (m.BaseColor / luminance_base) : (float3)1.0f;
        float3 r0_tinted = tint * fresnel_schlick_r0(eta_it);
        f_schlick += (1.0f - m.Metallic) * m.SpecularTint *
                     fresnel_schlick(r0_tinted, cos_theta_i, m.Eta);
    }

    float3 f_front =
        (1.0f - m.Metallic) * (1.0f - m.SpecularTint) * f_dielectric + f_schlick;
    return front_side ? f_front : (float3)(bsdf_weight * f_dielectric);
}

/// 求值 Principled BSDF 的反射部分。方向约定见文件头。
/// 返回已含 cos_theta_o 投影的辐亮度系数, 直接乘辐照度即可。
float3 eval_principled_reflection(PrincipledMaterial m, float3 wi, float3 wo) {
    float cos_theta_i = wi.z;
    float cos_theta_o = wo.z;
    if (cos_theta_i == 0.0f) {
        return (float3)0.0f;
    }

    // 只处理同侧的反射, 且只处理正面 —— 透射与背面由别的瓣负责。
    bool is_reflection = cos_theta_i * cos_theta_o > 0.0f;
    bool front_side = cos_theta_i > 0.0f;
    if (!is_reflection || !front_side) {
        return (float3)0.0f;
    }

    // 能量在三个瓣之间的分配: 介电体反射 brdf / 介电体透射 bsdf / 金属 (剩余)。
    float brdf = (1.0f - m.Metallic) * (1.0f - m.SpecularTrans);
    float bsdf = (1.0f - m.Metallic) * m.SpecularTrans;

    // 半程向量翻到法线正侧, 使 D/G 项的 m.z > 0 前提成立。
    float3 wh = normalize(wi + wo);
    wh *= (wh.z < 0.0f) ? -1.0f : 1.0f;

    float cos_theta_t;
    float eta_it;
    float eta_ti;
    float f_dielectric = fresnel_dielectric(dot(wi, wh), m.Eta, cos_theta_t, eta_it, eta_ti);
    // 两个方向都得在微表面的可见侧, 否则这个 wh 对应的是不存在的几何配置。
    float3 wh_oriented = wh * sign(cos_theta_i);
    bool reflection_valid = dot(wi, wh_oriented) > 0.0f && dot(wo, wh_oriented) > 0.0f;

    float3 value = (float3)0.0f;

    // ── 主镜面瓣 ──
    float2 alpha = roughness_to_alpha(m.Roughness, m.Anisotropy, m.Anisotropy > 0.0f);
    if (reflection_valid && f_dielectric > 0.0f) {
        float d = D_GGX(wh, alpha.x, alpha.y);
        float g = G_SmithGGX(wi, wo, alpha.x, alpha.y);
        float lum = (m.SpecularTint > 0.0f) ? luminance(m.BaseColor) : 1.0f;
        float3 f = principled_fresnel(m, f_dielectric, dot(wi, wh), lum, bsdf, front_side);
        // 1/(4 cos_i cos_o) 里的 cos_o 留给调用方约掉 (见文件头)。
        value += f * d * g / max(4.0f * abs(cos_theta_i), RADRAY_EPS);
    }

    // ── 清漆层 ──
    if (m.Clearcoat > 0.0f && reflection_valid) {
        float alpha_cc = lerp(0.1f, 0.001f, m.ClearcoatGloss);
        float f_cc = fresnel_schlick(0.04f, dot(wi, wh), m.Eta);
        float d_cc = D_GTR1(wh, alpha_cc);
        // Disney 固定用 0.25 求清漆的 G, 与 D 用的 alpha 无关。
        float g_cc = G_SmithGGX_Clearcoat(wi, wo, wh, 0.25f);
        value += (m.Clearcoat * 0.25f) * f_cc * d_cc * g_cc * abs(cos_theta_o);
    }

    // ── 漫反射 + 逆反射 + 次表面近似 + 绢光 ──
    if (brdf > 0.0f) {
        float f_o = fresnel_schlick_weight(abs(cos_theta_o));
        float f_i = fresnel_schlick_weight(abs(cos_theta_i));
        // Disney 漫反射: 掠射处压暗, 补一项与粗糙度相关的逆反射。
        float f_diffuse = (1.0f - 0.5f * f_i) * (1.0f - 0.5f * f_o);
        float cos_theta_d = dot(wh, wo);
        float rr = 2.0f * m.Roughness * cos_theta_d * cos_theta_d;
        float f_retro = rr * (f_o + f_i + f_o * f_i * (rr - 1.0f));
        float diffuse_term = f_diffuse + f_retro;

        if (m.Flatness > 0.0f) {
            // Hanrahan-Krueger 风格的薄次表面近似, 代替真正的 BSSRDF。
            float f_ss90 = rr * 0.5f;
            float f_ss = lerp(1.0f, f_ss90, f_o) * lerp(1.0f, f_ss90, f_i);
            float ss = 1.25f * (f_ss * (rcp(max(abs(cos_theta_o) + abs(cos_theta_i), RADRAY_EPS)) - 0.5f) + 0.5f);
            diffuse_term = lerp(diffuse_term, ss, m.Flatness);
        }
        value += brdf * abs(cos_theta_o) * m.BaseColor * RADRAY_INV_PI * diffuse_term;

        if (m.Sheen > 0.0f && (1.0f - m.Metallic) > 0.0f) {
            float f_d = fresnel_schlick_weight(abs(cos_theta_d));
            float3 c_sheen = (float3)1.0f;
            if (m.SheenTint > 0.0f) {
                float lum = luminance(m.BaseColor);
                float3 tint = (lum > 0.0f) ? (m.BaseColor / lum) : (float3)1.0f;
                c_sheen = lerp((float3)1.0f, tint, m.SheenTint);
            }
            value += m.Sheen * (1.0f - m.Metallic) * f_d * c_sheen * abs(cos_theta_o);
        }
    }

    return max(value, 0.0f);
}

#endif
