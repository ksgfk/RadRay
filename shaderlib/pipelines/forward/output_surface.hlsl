#include <core/platform.hlsli>
#include <core/color.hlsli>

struct OutputSurfaceData {
    float4x4 LocalToClip;
    float4 Options; // brightness, decode sRGB, reserved
};
VK_BINDING(0, 0) ConstantBuffer<OutputSurfaceData> OutputSurface : register(b0);
VK_BINDING(1, 0) Texture2D<float4> SceneOutput : register(t0);
VK_BINDING(2, 0) SamplerState OutputSampler : register(s0);
struct Varying { float4 Position : SV_Position; float2 UV : TEXCOORD0; };
[shader("vertex")]
Varying VSMain(uint id : SV_VertexID) {
    const float2 positions[6] = {float2(-.5, .5), float2(.5, .5), float2(.5, -.5),
                                float2(-.5, .5), float2(.5, -.5), float2(-.5, -.5)};
    Varying o;
    o.Position = mul(OutputSurface.LocalToClip, float4(positions[id], 0, 1));
    o.UV = positions[id] * float2(1, -1) + .5;
    return o;
}
[shader("pixel")]
float4 PSMain(Varying input) : SV_Target0 {
    float3 color = SceneOutput.Sample(OutputSampler, input.UV).rgb;
    if (OutputSurface.Options.y != 0) color = srgb_to_linear(color);
    return float4(color * OutputSurface.Options.x, 1);
}
