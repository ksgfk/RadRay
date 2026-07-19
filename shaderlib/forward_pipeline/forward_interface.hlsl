#ifndef RADRAY_FORWARD_INTERFACE_HLSL
#define RADRAY_FORWARD_INTERFACE_HLSL

#include "common.hlsl"
#include "forward_pipeline/binding_abi.hlsl"
#include "light.hlsl"
#include "point_shadow.hlsl"
#include "cascade_shadow.hlsl"

// The Forward ABI owns all three groups. Generic shader/material code only
// consumes the material group declared by the shader manifest.
struct PerObject {
    float4x4 ObjectToWorld;
};

struct ViewConstants {
    float4x4 ViewProj;
    float4 CameraPosition;
    // x = point light count, y = shadow point-light index+1,
    // z = directional light count, w = directional-shadow light index+1.
    uint4 LightCounts;
    PointLightGpu PointLights[RR_MAX_POINT_LIGHTS];
    DirectionalLightGpu DirectionalLights[RR_MAX_DIRECTIONAL_LIGHTS];
    PointShadowData PointShadow;
    ShadowParam DirectionalShadow;
};

VK_BINDING(1, RADRAY_FORWARD_OBJECT_BINDING_GROUP)
ConstantBuffer<PerObject> gPerObject : register(b1, RADRAY_FORWARD_OBJECT_SPACE);
VK_BINDING(0, RADRAY_FORWARD_PIPELINE_BINDING_GROUP)
ConstantBuffer<ViewConstants> gView : register(b0, RADRAY_FORWARD_PIPELINE_SPACE);

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
