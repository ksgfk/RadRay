#ifndef LIGHT_HLSLI
#define LIGHT_HLSLI

struct PointLightParams
{
    float3 position;
    float3 intensity;
};

float3 eval_point_light(PointLightParams light, float3 posW)
{
    float3 d = light.position - posW;
    float dist2 = dot(d, d);
    return light.intensity / dist2;
}

#endif
