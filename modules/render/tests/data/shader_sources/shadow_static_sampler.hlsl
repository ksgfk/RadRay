#include <core/platform.hlsli>

// The policy is declared for both targets on purpose: it is the single authority the compiler
// lowers into a D3D12 static sampler and into a Vulkan full-state immutable sampler record.
// It has to appear on every entry, byte-identical, so no stage compiles without it.
#define RS \
    "DescriptorTable(SRV(t0))," \
    "StaticSampler(s0, filter=FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT)"

VK_BINDING(1, 4)
Texture2D<float> ShadowTexture : register(t0);
VK_BINDING(2, 4)
SamplerComparisonState ShadowSampler : register(s0);

[shader("vertex")]
[RootSignature(RS)]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
[RootSignature(RS)]
float4 PSMain(float2 uv : TEXCOORD0) : SV_Target0 {
    return ShadowTexture.SampleCmpLevelZero(ShadowSampler, uv, 0.5).xxxx;
}
