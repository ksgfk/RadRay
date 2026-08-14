#include <core/platform.hlsli>

VK_BINDING(1, 4)
Texture2D<float> ShadowTexture : register(t0);
VK_BINDING(2, 4)
SamplerComparisonState ShadowSampler : register(s0);

#if !defined(__spirv__)
[RootSignature("DescriptorTable(SRV(t0)), StaticSampler(s0, filter=FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT)")]
#endif
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain(float2 uv : TEXCOORD0) : SV_Target0 {
    return ShadowTexture.SampleCmpLevelZero(ShadowSampler, uv, 0.5).xxxx;
}
