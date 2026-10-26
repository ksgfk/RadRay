#include <core/platform.hlsli>

VK_BINDING(2, 5)
Texture2D<float4> TargetTexture : register(t0);
VK_BINDING(3, 5)
SamplerState TargetSampler : register(s0);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain(float2 uv : TEXCOORD0) : SV_Target0 {
    return TargetTexture.SampleLevel(TargetSampler, uv, 0.0);
}
