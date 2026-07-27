#ifndef BSDF_HLSLI
#define BSDF_HLSLI

#include "common.hlsl"

float fresnel_dielectric(float cos_theta_i, float eta,
                         out float cos_theta_t, out float eta_it, out float eta_ti)
{
    bool outside = cos_theta_i >= 0.0;
    float rcp_eta = 1.0 / eta;
    eta_it = outside ? eta : rcp_eta;
    eta_ti = outside ? rcp_eta : eta;

    float cos_theta_i_abs = abs(cos_theta_i);
    float cos_theta_t_sqr = 1.0 - (1.0 - cos_theta_i_abs * cos_theta_i_abs) * (eta_ti * eta_ti);
    float cos_theta_t_abs = sqrt(max(cos_theta_t_sqr, 0.0));

    float a_s = (eta_it * cos_theta_i_abs - cos_theta_t_abs) /
                (eta_it * cos_theta_i_abs + cos_theta_t_abs);
    float a_p = (eta_it * cos_theta_t_abs - cos_theta_i_abs) /
                (eta_it * cos_theta_t_abs + cos_theta_i_abs);

    float r = 0.5 * (a_s * a_s + a_p * a_p);

    bool index_matched = (eta == 1.0);
    bool special_case = index_matched || (cos_theta_i_abs == 0.0);
    float r_sc = index_matched ? 0.0 : 1.0;
    r = special_case ? r_sc : r;

    cos_theta_t = (cos_theta_t_sqr >= 0.0) ? -sign(cos_theta_i) * cos_theta_t_abs : 0.0;
    return r;
}

float D_GGX_aniso(float3 m, float ax, float ay)
{
    if (m.z <= 0.0)
        return 0.0;

    float sx = m.x / ax;
    float sy = m.y / ay;
    float sz = m.z;
    float denom = sx * sx + sy * sy + sz * sz;
    float d = 1.0 / (PI * ax * ay * denom * denom);
    return max(d, 0.0);
}

float smith_GGX1_aniso(float3 v, float ax, float ay)
{
    if (v.z <= 0.0)
        return 0.0;

    float vx = v.x * ax;
    float vy = v.y * ay;
    float vz = v.z;
    float tan2 = (vx * vx + vy * vy) / (vz * vz);
    float lambda = (-1.0 + sqrt(1.0 + tan2)) * 0.5;
    return 1.0 / (1.0 + lambda);
}

float G_GGX_aniso(float3 wi, float3 wo, float ax, float ay)
{
    return smith_GGX1_aniso(wi, ax, ay) * smith_GGX1_aniso(wo, ax, ay);
}

#endif
