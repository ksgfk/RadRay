#ifndef LIGHT_HLSLI
#define LIGHT_HLSLI

#define RR_MAX_DIRECTIONAL_LIGHTS 8
#define RR_MAX_POINT_LIGHTS 64

struct DirectionalLightGpu {
    // Direction.xyz is the normalized light travel direction, from light into the scene.
    float4 Direction;
    float4 Irradiance;
};

struct PointLightGpu {
    float4 Position;
    float4 Intensity;
};

float3 eval_directional_irradiance(DirectionalLightGpu light) {
    return light.Irradiance.rgb;
}

float3 eval_point_irradiance(PointLightGpu light, float3 posW) {
    float3 d = light.Position.xyz - posW;
    float dist2 = max(dot(d, d), 1e-6f);
    return light.Intensity.rgb / dist2;
}

#endif
