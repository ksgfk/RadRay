#ifndef RADRAY_FORWARD_INTERFACE_HLSL
#define RADRAY_FORWARD_INTERFACE_HLSL

#include "common.hlsl"
#include "forward_pipeline/binding_abi.hlsl"
#include "light.hlsl"
#include "point_shadow.hlsl"
#include "cascade_shadow.hlsl"

// Forward providers reserve object/pipeline groups. The material group is a
// shader convention and remains user-owned because the policy does not reserve it.
// per-object 常量 (PerObject / gPerObject) 由 binding_abi.hlsl 提供。

struct ViewConstants {
    float4x4 ViewProj;
    float4 CameraPosition;
    // x = point light count, y = shadow point-light index+1,
    // z = directional light count, w = directional-shadow light index+1.
    uint4 LightCounts;
    PointLightGpu PointLights[RADRAY_MAX_POINT_LIGHTS];
    DirectionalLightGpu DirectionalLights[RADRAY_MAX_DIRECTIONAL_LIGHTS];
    PointShadowData PointShadow;
    CascadeShadowData DirectionalShadow;
};

VK_BINDING(0, RADRAY_FORWARD_PIPELINE_BINDING_GROUP)
ConstantBuffer<ViewConstants> gView : register(b0, RADRAY_FORWARD_PIPELINE_SPACE);

// 阴影绑定的 keyword 声明就放在它们守护的 #ifdef 旁边 —— 这是唯一不会失同步的位置。
// tools/shader_gen 从预处理输出里读这两条, 故每个 include 本文件的入口 shader 自动
// 继承这两个变体维度 (详见 runtime/shader_asset_template.h)。
#pragma radray_keyword_group(PointShadows, _POINT_SHADOWS) stages(Pixel)
#pragma radray_keyword_group(DirectionalShadows, _DIRECTIONAL_SHADOWS) stages(Pixel)

#ifdef _POINT_SHADOWS
    VK_BINDING(1, RADRAY_FORWARD_PIPELINE_BINDING_GROUP)
    TextureCube<float> gShadowCube : register(t1, RADRAY_FORWARD_PIPELINE_SPACE);
#endif
#ifdef _DIRECTIONAL_SHADOWS
    VK_BINDING(2, RADRAY_FORWARD_PIPELINE_BINDING_GROUP)
    Texture2DArray<float> gShadowArray : register(t2, RADRAY_FORWARD_PIPELINE_SPACE);
#endif
#if defined(_POINT_SHADOWS) || defined(_DIRECTIONAL_SHADOWS)
    VK_BINDING(3, RADRAY_FORWARD_PIPELINE_BINDING_GROUP)
    SamplerComparisonState gShadowSampler : register(s3, RADRAY_FORWARD_PIPELINE_SPACE);
#endif

// Forward material bindings use one namespace across D3D registers and
// Vulkan bindings. Binding 0 is reserved for the material constant buffer.
#define RADRAY_FORWARD_MATERIAL_CBUFFER(type, name) \
    VK_BINDING(0, RADRAY_FORWARD_MATERIAL_BINDING_GROUP) \
    ConstantBuffer<type> name : register(b0, RADRAY_FORWARD_MATERIAL_SPACE)

#define RADRAY_FORWARD_MATERIAL_TEXTURE2D(name, slot, binding) \
    VK_BINDING(binding, RADRAY_FORWARD_MATERIAL_BINDING_GROUP) \
    Texture2D name : register(t##slot, RADRAY_FORWARD_MATERIAL_SPACE)

#define RADRAY_FORWARD_MATERIAL_SAMPLER(name, slot, binding) \
    VK_BINDING(binding, RADRAY_FORWARD_MATERIAL_BINDING_GROUP) \
    SamplerState name : register(s##slot, RADRAY_FORWARD_MATERIAL_SPACE)

#endif
