#ifndef RADRAY_PIPELINES_FORWARD_BINDINGS_HLSLI
#define RADRAY_PIPELINES_FORWARD_BINDINGS_HLSLI

#include <core/platform.hlsli>
#include <lighting/lights.hlsli>

struct ForwardViewData {
    float4x4 ViewProj;
    float4 EyePosition;
    uint DirectionalLightCount;
    uint PointLightCount;
    float2 LightCountPadding;
    DirectionalLight DirectionalLights[RADRAY_MAX_DIRECTIONAL_LIGHTS];
    PointLight PointLights[RADRAY_MAX_POINT_LIGHTS];
};

struct ForwardMaterialData {
    float4 BaseColor;
};

struct ForwardObjectData {
    float4x4 LocalToWorld;
};

VK_BINDING(0, 0)
ConstantBuffer<ForwardViewData> ForwardView : register(b0, space0);

VK_BINDING(0, 1)
ConstantBuffer<ForwardMaterialData> ForwardMaterial : register(b0, space1);

VK_BINDING(1, 1)
Texture2D<float4> AlbedoTexture : register(t0, space1);
VK_BINDING(2, 1)
SamplerState LinearSampler : register(s0, space1);

VK_BINDING(0, 2)
ConstantBuffer<ForwardObjectData> ForwardObject : register(b0, space2);

#endif
