#include <core/platform.hlsli>
#include <core/color.hlsli>
struct UiData { float4 Transform; float4 Options; };
VK_BINDING(0, 0) ConstantBuffer<UiData> Ui : register(b0);
VK_BINDING(1, 0) Texture2D<float4> Image : register(t0);
VK_BINDING(2, 0) SamplerState ImageSampler : register(s0);
struct Vertex {
    VK_LOCATION(0) float2 Position : POSITION;
    VK_LOCATION(1) float2 UV : TEXCOORD0;
    VK_LOCATION(2) float4 Color : COLOR0;
};
struct Fragment { float4 Position : SV_Position; float2 UV : TEXCOORD0; float4 Color : COLOR0; };
[shader("vertex")] Fragment VSMain(Vertex v) {
    Fragment o; o.Position = float4(v.Position * Ui.Transform.xy + Ui.Transform.zw, 0, 1);
    o.UV = v.UV; o.Color = float4(srgb_to_linear(v.Color.rgb), v.Color.a); return o;
}
[shader("pixel")] float4 PSMain(Fragment v) : SV_Target0 {
    float4 sampled = Image.Sample(ImageSampler, v.UV);
    if (Ui.Options.x > .5) sampled.rgb = srgb_to_linear(sampled.rgb);
    return sampled * v.Color;
}
