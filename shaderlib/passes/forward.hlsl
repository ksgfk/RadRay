#include <core/color.hlsli>
#include <core/platform.hlsli>

#pragma radray_keyword_group QUALITY "low" "high"

VK_BINDING(6, 2)
Texture2D<float4> AlbedoTexture;
VK_BINDING(7, 2)
SamplerState LinearSampler;

struct ForwardVertexInput {
    float3 Position : POSITION;
    float2 UV : TEXCOORD0;
};

struct ForwardVertexOutput {
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

[shader("vertex")]
ForwardVertexOutput VSMain(ForwardVertexInput input) {
    ForwardVertexOutput output;
    output.Position = float4(input.Position, 1.0f);
    output.UV = input.UV;
    return output;
}

[shader("pixel")]
float4 PSMain(ForwardVertexOutput input) : SV_Target0 {
    const float3 linearColor = AlbedoTexture.SampleLevel(LinearSampler, input.UV, 0.0f).rgb;
    return float4(linear_to_srgb(tonemap_reinhard_luminance(linearColor)), 1.0f);
}
