#ifndef RADRAY_LIGHTING_LIGHTS_HLSLI
#define RADRAY_LIGHTING_LIGHTS_HLSLI

#include <core/math.hlsli>

// 光源的 GPU 布局与辐照度求值。
//
// 这些 struct 是 CPU/GPU 共享的 ABI: 字段顺序与 float4 打包方式必须与 CPU 端逐字段对齐,
// 改动前先确认写入侧。数组上限也是 ABI 的一部分 (决定 cbuffer 大小)。

#define RADRAY_MAX_DIRECTIONAL_LIGHTS 8
#define RADRAY_MAX_POINT_LIGHTS 8

struct DirectionalLight {
    // xyz = 归一化的光【传播】方向 (光源射向场景), w 保留。
    // 求"指向光源"的方向需取反, 别直接拿来当 L。
    float4 Direction;
    float4 Irradiance;  // rgb = 垂直入射面上的辐照度, w 保留
};

struct PointLight {
    float4 Position;   // xyz = 世界位置, w = 影响半径
    float4 Intensity;  // rgb = 辐射强度 (radiant intensity), w 保留
};

/// 方向光: 平行光无衰减, 辐照度与位置无关。
float3 eval_directional_irradiance(DirectionalLight light) {
    return light.Irradiance.rgb;
}

/// 指向光源的单位方向 (即着色用的 L)。
float3 directional_light_direction(DirectionalLight light) {
    return normalize(-light.Direction.xyz);
}

/// 点光源: 平方反比衰减。分母下限防止贴到光源位置时溢出。
float3 eval_point_irradiance(PointLight light, float3 position_world) {
    float3 to_light = light.Position.xyz - position_world;
    float dist2 = max(dot(to_light, to_light), RADRAY_EPS);
    return light.Intensity.rgb / dist2;
}

#endif
