#ifndef RADRAY_FORWARD_EFFECTS_HLSLI
#define RADRAY_FORWARD_EFFECTS_HLSLI
#include <core/platform.hlsli>
#include <core/color.hlsli>
#include <pipelines/forward/local_light.hlsli>
struct EffectsData {
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
VK_BINDING(0, 0) ConstantBuffer<EffectsData> Effects : register(b0);
VK_BINDING(1, 0) Texture2D<float4> InputA : register(t0);
VK_BINDING(2, 0) Texture2D<float4> InputB : register(t1);
VK_BINDING(3, 0) Texture2D<float4> InputC : register(t2);
VK_BINDING(4, 0) Texture2D<float4> InputD : register(t3);
VK_BINDING(5, 0) Texture2D<float4> InputE : register(t4);
VK_BINDING(6, 0) SamplerState ClampSampler : register(s0);
VK_IMAGE_FORMAT("rgba16f") VK_BINDING(7, 0) RWTexture2D<float4> OutputColor : register(u0);
VK_BINDING(8, 0) RWTexture2D<float> OutputScalar : register(u1);
VK_BINDING(9, 0) StructuredBuffer<ForwardLocalLight> Lights : register(t5);
VK_BINDING(10, 0) RWStructuredBuffer<uint2> Headers : register(u2);
VK_BINDING(11, 0) RWStructuredBuffer<uint> Indices : register(u3);

float2 uv_of(uint2 pixel) { return (pixel + .5) / Effects.Extent.xy; }
float3 view_position(float2 uv, float z) {
    float4 p = mul(Effects.InverseProjection, float4(uv * float2(2, -2) + float2(-1, 1), z, 1));
    return p.xyz / p.w;
}
float3 sample_a(float2 uv) { return InputA.SampleLevel(ClampSampler, saturate(uv), 0).rgb; }

#endif
