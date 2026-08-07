#include <core/platform.hlsli>

VK_BINDING(7, 1)
Texture2D<float4> AlbedoTexture;
VK_BINDING(8, 1)
SamplerState LinearSampler;

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain(float2 uv : TEXCOORD0) : SV_Target0 {
    return AlbedoTexture.Sample(LinearSampler, uv);
}
