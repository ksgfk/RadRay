#include <core/platform.hlsli>
#include <core/color.hlsli>
struct UiData { float4 Transform; float4 Options; };
VK_BINDING(0, 0) ConstantBuffer<UiData> Ui : register(b0);
VK_BINDING(1, 0) Texture2D<float4> Image : register(t0);
VK_BINDING(2, 0) SamplerState ImageSampler : register(s0);
struct Fragment { float4 Position : SV_Position; float2 UV : TEXCOORD0; };
[shader("vertex")] Fragment VSMain(uint id : SV_VertexID) {
    Fragment o; o.UV = float2(id == 2 ? 2 : 0, id == 1 ? 2 : 0);
    o.Position = float4(o.UV * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
[shader("pixel")] float4 PSMain(Fragment v) : SV_Target0 {
    float4 value = Image.SampleLevel(ImageSampler, v.UV, 0);
    if (Ui.Options.x > .5) value.rgb = srgb_to_linear(value.rgb);
    if (Ui.Options.y > .5) value.rgb = linear_to_srgb(saturate(value.rgb));
    return value;
}
