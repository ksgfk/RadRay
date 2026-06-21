#ifndef PRINCIPLED_HLSLI
#define PRINCIPLED_HLSLI

#include "bsdf.hlsl"

float luminance3(float3 c) {
    return dot(c, float3(0.212671f, 0.715160f, 0.072169f));
}

float schlick_weight(float cos_i) {
    float m = saturate(1.0f - cos_i);
    float m2 = m * m;
    return m2 * m2 * m;
}

float schlick_R0_eta(float eta) {
    float v = (eta - 1.0f) / (eta + 1.0f);
    return v * v;
}

float dielectric_schlick_weight(float cos_theta_i, float eta) {
    float cos_theta_t = 0.0f;
    float eta_it = 1.0f;
    float eta_ti = 1.0f;
    fresnel_dielectric(cos_theta_i, eta, cos_theta_t, eta_it, eta_ti);
    return (eta_it > 1.0f)
        ? schlick_weight(abs(cos_theta_i))
        : schlick_weight(abs(cos_theta_t));
}

float calc_schlick(float R0, float cos_theta_i, float eta) {
    return lerp(dielectric_schlick_weight(cos_theta_i, eta), 1.0f, R0);
}

float3 calc_schlick3(float3 R0, float cos_theta_i, float eta) {
    return lerp(dielectric_schlick_weight(cos_theta_i, eta).xxx, 1.0f.xxx, R0);
}

float2 calc_dist_params(float anisotropic, float roughness, bool has_anisotropic) {
    float roughness2 = roughness * roughness;
    if (!has_anisotropic) {
        float a = max(0.001f, roughness2);
        return float2(a, a);
    }
    float aspect = sqrt(max(0.001f, 1.0f - 0.9f * anisotropic));
    return float2(max(0.001f, roughness2 / aspect), max(0.001f, roughness2 * aspect));
}

float gtr1_D(float3 h, float alpha) {
    float cos_theta = h.z;
    if (cos_theta <= 0.0f) {
        return 0.0f;
    }
    float alpha2 = alpha * alpha;
    float denom = PI * log(alpha2) * (1.0f + (alpha2 - 1.0f) * cos_theta * cos_theta);
    float result = (alpha2 - 1.0f) / denom;
    return (result * cos_theta > 1e-20f) ? result : 0.0f;
}

float smith_ggx1_clearcoat(float3 v, float3 h, float alpha) {
    float alpha2 = alpha * alpha;
    float cos_theta = abs(v.z);
    float cos_theta2 = cos_theta * cos_theta;
    if (cos_theta2 <= 0.0f) {
        return 0.0f;
    }
    float tan_theta2 = (1.0f - cos_theta2) / cos_theta2;
    float result = 2.0f * rcp(1.0f + sqrt(1.0f + alpha2 * tan_theta2));
    if (v.z == 1.0f) {
        result = 1.0f;
    }
    if (dot(v, h) * v.z <= 0.0f) {
        result = 0.0f;
    }
    return result;
}

float clearcoat_G(float3 wi, float3 wo, float3 h, float alpha) {
    return smith_ggx1_clearcoat(wi, h, alpha) * smith_ggx1_clearcoat(wo, h, alpha);
}

float3 principled_fresnel(float F_dielectric, float metallic, float spec_tint, float3 base_color, float lum,
                          float cos_theta_i, bool front_side, float bsdf, float eta,
                          bool has_metallic, bool has_spec_tint) {
    float cos_theta_t = 0.0f;
    float eta_it = 1.0f;
    float eta_ti = 1.0f;
    fresnel_dielectric(cos_theta_i, eta, cos_theta_t, eta_it, eta_ti);
    float3 F_schlick = float3(0.0f, 0.0f, 0.0f);

    if (has_metallic) {
        F_schlick += metallic * calc_schlick3(base_color, cos_theta_i, eta);
    }

    if (has_spec_tint) {
        float3 c_tint = lum > 0.0f ? base_color / lum : float3(1.0f, 1.0f, 1.0f);
        float3 F0_spec_tint = c_tint * schlick_R0_eta(eta_it);
        F_schlick += (1.0f - metallic) * spec_tint * calc_schlick3(F0_spec_tint, cos_theta_i, eta);
    }

    float3 F_front = (1.0f - metallic) * (1.0f - spec_tint) * F_dielectric + F_schlick;
    return front_side ? F_front : (bsdf * F_dielectric).xxx;
}

// 与 Mitsuba3 src/bsdfs/principled.cpp 的 eval() 逐项对齐,参数方向约定必须严格一致:
//   wi = 视线方向(指向相机)= Mitsuba si.wi   -> cos_theta_i = wi.z
//   wo = 光源方向(指向光源)= Mitsuba eval 第二参 wo -> cos_theta_o = wo.z
// 这与“wi 指向光源”的常见习惯相反!函数返回值已含出射侧余弦投影:
//   - 漫反射/清漆/绢光乘 abs(cos_theta_o)（即 N·L_light）
//   - 镜面反射只除 abs(cos_theta_i)（拆走了 1/(4 cos_i cos_o) 中的 cos_o）
// 因此调用方无需(也不得)再乘 N·L；调换 wi/wo 会同时弄错高光归一化与终止线。
float3 EvalPrincipledReflection(float3 wi, float3 wo, float3 base_color, float metallic, float roughness,
                                float specular, float spec_tint, float anisotropic, float sheen,
                                float sheen_tint, float flatness, float clearcoat, float clearcoat_gloss,
                                float spec_trans, float eta) {
    float cos_theta_i = wi.z;
    if (cos_theta_i == 0.0f) {
        return 0.0f;
    }

    float brdf = (1.0f - metallic) * (1.0f - spec_trans);
    float bsdf = (1.0f - metallic) * spec_trans;
    float cos_theta_o = wo.z;
    bool reflect = cos_theta_i * cos_theta_o > 0.0f;
    bool front_side = cos_theta_i > 0.0f;
    if (!reflect || !front_side) {
        return 0.0f;
    }

    float3 wh = normalize(wi + wo);
    wh *= wh.z < 0.0f ? -1.0f : 1.0f;

    float cos_theta_t = 0.0f;
    float eta_it = 1.0f;
    float eta_ti = 1.0f;
    float F_dielectric = fresnel_dielectric(dot(wi, wh), eta, cos_theta_t, eta_it, eta_ti);
    bool reflection_compat = dot(wi, wh * sign(cos_theta_i)) > 0.0f && dot(wo, wh * sign(cos_theta_i)) > 0.0f;

    float2 a = calc_dist_params(anisotropic, roughness, anisotropic > 0.0f);
    float D = D_GGX_aniso(wh, a.x, a.y);
    float G = G_GGX_aniso(wi, wo, a.x, a.y);
    float3 value = 0.0f;

    if (reflection_compat && F_dielectric > 0.0f) {
        float lum = spec_tint > 0.0f ? luminance3(base_color) : 1.0f;
        float3 Fp = principled_fresnel(F_dielectric, metallic, spec_tint, base_color, lum,
                                       dot(wi, wh), front_side, bsdf, eta,
                                       metallic > 0.0f, spec_tint > 0.0f);
        value += Fp * D * G / max(4.0f * abs(cos_theta_i), 1e-6f);
    }

    if (clearcoat > 0.0f && reflection_compat) {
        float alpha_cc = lerp(0.1f, 0.001f, clearcoat_gloss);
        float Fcc = calc_schlick(0.04f, dot(wi, wh), eta);
        float Dcc = gtr1_D(wh, alpha_cc);
        float Gcc = clearcoat_G(wi, wo, wh, 0.25f);
        value += (clearcoat * 0.25f) * Fcc * Dcc * Gcc * abs(cos_theta_o);
    }

    if (brdf > 0.0f) {
        float Fo = schlick_weight(abs(cos_theta_o));
        float Fi = schlick_weight(abs(cos_theta_i));
        float f_diff = (1.0f - 0.5f * Fi) * (1.0f - 0.5f * Fo);
        float cos_theta_d = dot(wh, wo);
        float Rr = 2.0f * roughness * cos_theta_d * cos_theta_d;
        float f_retro = Rr * (Fo + Fi + Fo * Fi * (Rr - 1.0f));
        float diffuse_term = f_diff + f_retro;
        if (flatness > 0.0f) {
            float Fss90 = Rr * 0.5f;
            float Fss = lerp(1.0f, Fss90, Fo) * lerp(1.0f, Fss90, Fi);
            float f_ss = 1.25f * (Fss * (rcp(max(abs(cos_theta_o) + abs(cos_theta_i), 1e-6f)) - 0.5f) + 0.5f);
            diffuse_term = lerp(diffuse_term, f_ss, flatness);
        }
        value += brdf * abs(cos_theta_o) * base_color * INV_PI * diffuse_term;

        if (sheen > 0.0f && (1.0f - metallic) > 0.0f) {
            float Fd = schlick_weight(abs(cos_theta_d));
            float3 c_sheen = 1.0f;
            if (sheen_tint > 0.0f) {
                float lum = luminance3(base_color);
                float3 c_tint = lum > 0.0f ? base_color / lum : 1.0f;
                c_sheen = lerp(1.0f.xxx, c_tint, sheen_tint);
            }
            value += sheen * (1.0f - metallic) * Fd * c_sheen * abs(cos_theta_o);
        }
    }

    return max(value, 0.0f);
}

#endif
