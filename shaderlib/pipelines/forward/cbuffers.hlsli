#ifndef RADRAY_PIPELINES_FORWARD_CBUFFERS_HLSLI
#define RADRAY_PIPELINES_FORWARD_CBUFFERS_HLSLI

#include <lighting/lights.hlsli>

// Forward pipeline cbuffer ABI. CPU generated PODs use the same identifiers.
// Binding declaration names stay ForwardView / ForwardMaterial / ForwardObject.

struct Forward_ViewData {
    float4x4 ViewProj;
    float4x4 PreviousViewProj;
    float4 EyePosition;
    uint DirectionalLightCount;
    uint PointLightCount;
    float2 LightCountPadding;
    DirectionalLight DirectionalLights[RADRAY_MAX_DIRECTIONAL_LIGHTS];
    PointLight PointLights[RADRAY_MAX_POINT_LIGHTS];
};

struct Forward_MaterialData {
    float4 BaseColor;
    // metallic, perceptual roughness, alpha cutoff, emission
    float4 Surface;
    // refraction offset in pixels, unlit, reserved
    float4 Transmission;
    // UV scale and offset; zero scale selects the identity scale.
    float4 UVTransform;
};

struct Forward_ObjectData {
    float4x4 LocalToWorld;
    float4x4 NormalToWorld;
    float4x4 PreviousLocalToWorld;
    uint MotionValid;
};

struct Forward_PassData {
    float4x4 ShadowMatrix0, ShadowMatrix1, ShadowMatrix2, ShadowMatrix3;
    float4 ShadowSphere[4];
    float4 ShadowBias[4];
    float4 ShadowParams;
    float4 Extent; // width, height, tilesX, per-tile capacity
    uint LocalLightCount;
    uint UseTiles;
    uint UseAo;
    uint Transparent;
};

struct Forward_EffectsData {
    float4x4 InverseProjection;
    float4x4 InverseViewProjection;
    float4x4 PreviousViewProjection;
    float4x4 WorldToView;
    float4x4 Projection;
    float4 Extent; // output xy, input zw
    float4 Options; // effect-specific controls
    float4 Eye;
    uint LocalLightCount;
    uint TileCapacity;
    uint HistoryValid;
    uint DebugMode;
};

struct Forward_OutputSurfaceData {
    float4x4 LocalToClip;
    float4 Options; // brightness, decode sRGB, reserved
};

#endif
