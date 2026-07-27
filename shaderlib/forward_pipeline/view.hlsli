#ifndef RADRAY_FORWARD_PIPELINE_VIEW_HLSLI
#define RADRAY_FORWARD_PIPELINE_VIEW_HLSLI

#include <forward_pipeline/bindings.hlsli>
#include <lighting/lights.hlsli>
#include <shadow/cascade.hlsli>
#include <shadow/cube.hlsli>

// forward 管线提供的 per-view 数据: 相机、光源列表、阴影资源。
//
// 管线保留 group 0 (object) 与 group 1 (view); group 2 (material) 归各 pass 自己声明。

struct ViewConstants {
    float4x4 ViewProj;
    float4 CameraPosition;  // xyz = 世界位置, w 保留
    // x = 点光源数量
    // y = 投阴影的点光源序号 + 1 (0 = 无)
    // z = 方向光数量
    // w = 投阴影的方向光序号 + 1 (0 = 无)
    // 用 +1 编码是为了让 0 表示"没有", 免得再占一个 flag 字段。
    uint4 LightCounts;
    PointLight PointLights[RADRAY_MAX_POINT_LIGHTS];
    DirectionalLight DirectionalLights[RADRAY_MAX_DIRECTIONAL_LIGHTS];
    CubeShadow PointShadow;
    CascadeShadow DirectionalShadow;
};

RADRAY_FORWARD_VIEW_CBUFFER(ViewConstants, gView, 0, 0);

// 阴影绑定的 keyword 声明就贴在它们守护的 #ifdef 旁边 —— 这是唯一不会失同步的位置。
// tools/shader_gen 从预处理输出里读这两条, 故每个包含本文件的入口 shader 自动继承这两个
// 变体维度 (详见 runtime/shader_asset_template.h)。
#pragma radray_keyword_group(PointShadows, _POINT_SHADOWS) stages(Pixel)
#pragma radray_keyword_group(DirectionalShadows, _DIRECTIONAL_SHADOWS) stages(Pixel)

#ifdef _POINT_SHADOWS
VK_BINDING(1, RADRAY_FORWARD_VIEW_GROUP)
TextureCube<float> gShadowCube : register(t1, RADRAY_FORWARD_VIEW_SPACE);
#endif

#ifdef _DIRECTIONAL_SHADOWS
VK_BINDING(2, RADRAY_FORWARD_VIEW_GROUP)
Texture2DArray<float> gShadowArray : register(t2, RADRAY_FORWARD_VIEW_SPACE);
#endif

#if defined(_POINT_SHADOWS) || defined(_DIRECTIONAL_SHADOWS)
VK_BINDING(3, RADRAY_FORWARD_VIEW_GROUP)
SamplerComparisonState gShadowSampler : register(s3, RADRAY_FORWARD_VIEW_SPACE);
#endif

/// 本帧实际参与着色的方向光数量 (夹到数组上限)。
uint view_directional_light_count() {
    return min(gView.LightCounts.z, (uint)RADRAY_MAX_DIRECTIONAL_LIGHTS);
}

/// 本帧实际参与着色的点光源数量 (夹到数组上限)。
uint view_point_light_count() {
    return min(gView.LightCounts.x, (uint)RADRAY_MAX_POINT_LIGHTS);
}

/// 第 index 盏方向光是否是那盏投级联阴影的。
bool view_is_shadowed_directional_light(uint index) {
    uint encoded = gView.LightCounts.w;
    return encoded != 0u && (index + 1u) == encoded;
}

/// 第 index 盏点光源是否是那盏投立方体阴影的。
bool view_is_shadowed_point_light(uint index) {
    uint encoded = gView.LightCounts.y;
    return encoded != 0u && (index + 1u) == encoded;
}

/// 方向光可见度。无 _DIRECTIONAL_SHADOWS 变体时整块采样被编译期剔除。
float view_directional_shadow(uint index, float3 position_world, float3 normal_world, float3 dir_to_light) {
#ifdef _DIRECTIONAL_SHADOWS
    if (view_is_shadowed_directional_light(index)) {
        return sample_cascade_shadow(
            gShadowArray, gShadowSampler, gView.DirectionalShadow,
            position_world, normal_world, dir_to_light);
    }
#endif
    return 1.0f;
}

/// 点光源可见度。无 _POINT_SHADOWS 变体时整块采样被编译期剔除。
float view_point_shadow(uint index, float3 position_world, float3 normal_world) {
#ifdef _POINT_SHADOWS
    if (view_is_shadowed_point_light(index)) {
        return sample_cube_shadow(
            gShadowCube, gShadowSampler, gView.PointShadow,
            position_world, normal_world);
    }
#endif
    return 1.0f;
}

#endif
