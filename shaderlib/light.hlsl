#ifndef LIGHT_HLSLI
#define LIGHT_HLSLI

#define RR_MAX_DIRECTIONAL_LIGHTS 8
#define RR_MAX_POINT_LIGHTS 8
#define RR_MAX_SPOT_LIGHTS 64

struct DirectionalLightGpu {
    // Direction.xyz is the normalized light travel direction, from light into the scene.
    float4 Direction;
    float4 Irradiance;
};

struct PointLightGpu {
    float4 Position;   // xyz position, w = range
    float4 Intensity;  // rgb radiance, w = shadow first-slice index (-1 = no shadow)
};

struct SpotLightGpu {
    float4 Position;   // xyz position, w = range
    float4 Direction;  // xyz normalized spot direction, w = cos(outer half-angle)
    float4 Intensity;  // rgb radiance, w = cos(inner half-angle)
    float4 Params;     // x = shadow slice index (-1 = no shadow), yzw reserved
};

float3 eval_directional_irradiance(DirectionalLightGpu light) {
    return light.Irradiance.rgb;
}

float3 eval_point_irradiance(PointLightGpu light, float3 posW) {
    float3 d = light.Position.xyz - posW;
    float dist2 = max(dot(d, d), 1e-6f);
    return light.Intensity.rgb / dist2;
}

// Spot irradiance: inverse-square distance * smooth cone falloff between inner and outer half-angles.
float3 eval_spot_irradiance(SpotLightGpu light, float3 posW) {
    float3 d = light.Position.xyz - posW;
    float dist2 = max(dot(d, d), 1e-6f);
    float3 toLight = d * rsqrt(dist2);
    // cos of angle between -spotDir and direction to the light = how aligned the surface is with the cone axis.
    float cosAngle = dot(-light.Direction.xyz, -toLight);
    float cosOuter = light.Direction.w;
    float cosInner = light.Intensity.w;
    float denom = max(cosInner - cosOuter, 1e-4f);
    float cone = saturate((cosAngle - cosOuter) / denom);
    cone *= cone;  // quadratic smoothing, matches URP's smooth spot edge
    return light.Intensity.rgb * (cone / dist2);
}

#endif
