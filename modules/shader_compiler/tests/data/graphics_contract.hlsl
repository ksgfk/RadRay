#include <core/platform.hlsli>

#pragma radray_keyword_group QUALITY "low" "high"

VK_BINDING(7, 1) Texture2D<float4> AlbedoTexture : register(t0);
VK_BINDING(8, 1) SamplerState AlbedoSampler : register(s0);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain(float2 uv : TEXCOORD0) : SV_Target0 {
    return AlbedoTexture.SampleLevel(AlbedoSampler, uv, 0.0);
}
