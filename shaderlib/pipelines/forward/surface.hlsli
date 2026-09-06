#ifndef RADRAY_FORWARD_SURFACE_HLSLI
#define RADRAY_FORWARD_SURFACE_HLSLI
#include <core/platform.hlsli>
#include <core/math.hlsli>
#include <lighting/lights.hlsli>

struct ForwardViewData {
    float4x4 ViewProj;
    float4x4 PreviousViewProj;
    float4 EyePosition;
    uint DirectionalLightCount;
    uint PointLightCount;
    float2 LightCountPadding;
    DirectionalLight DirectionalLights[RADRAY_MAX_DIRECTIONAL_LIGHTS];
    PointLight PointLights[RADRAY_MAX_POINT_LIGHTS];
};
struct ForwardMaterialData {
    float4 BaseColor;
    // metallic, perceptual roughness, alpha cutoff, emission
    float4 Surface;
    // refraction offset in pixels, reserved
    float4 Transmission;
};
struct ForwardObjectData {
    float4x4 LocalToWorld;
    float4x4 NormalToWorld;
    float4x4 PreviousLocalToWorld;
    uint MotionValid;
};
VK_BINDING(0, 0) ConstantBuffer<ForwardViewData> ForwardView : register(b0, space0);
VK_BINDING(0, 1) ConstantBuffer<ForwardMaterialData> ForwardMaterial : register(b0, space1);
VK_BINDING(1, 1) Texture2D<float4> AlbedoTexture : register(t0, space1);
VK_BINDING(2, 1) SamplerState LinearSampler : register(s0, space1);
VK_BINDING(0, 2) ConstantBuffer<ForwardObjectData> ForwardObject : register(b0, space2);

struct SurfaceVertexInput { float3 Position : POSITION; float3 Normal : NORMAL; float2 UV : TEXCOORD0; };
struct SurfaceVertexOutput {
    float4 Position : SV_Position;
    float3 WorldPosition : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float2 UV : TEXCOORD2;
    float4 CurrentClip : TEXCOORD3;
    float4 PreviousClip : TEXCOORD4;
};
SurfaceVertexOutput forward_surface_vertex(SurfaceVertexInput v) {
    SurfaceVertexOutput o;
    float4 world = mul(ForwardObject.LocalToWorld, float4(v.Position, 1));
    o.Position = mul(ForwardView.ViewProj, world);
    o.CurrentClip = o.Position;
    o.PreviousClip = mul(ForwardView.PreviousViewProj, mul(ForwardObject.PreviousLocalToWorld, float4(v.Position, 1)));
    o.WorldPosition = world.xyz;
    o.Normal = safe_normalize(mul((float3x3)ForwardObject.NormalToWorld, v.Normal), float3(0, 1, 0));
    o.UV = v.UV;
    return o;
}
float4 forward_surface_color(float2 uv) {
    float4 color = AlbedoTexture.Sample(LinearSampler, uv) * ForwardMaterial.BaseColor;
    clip(color.a - ForwardMaterial.Surface.z);
    return color;
}
float2 forward_clip_uv(float4 p) { return p.xy / p.w * float2(.5, -.5) + .5; }
#endif
