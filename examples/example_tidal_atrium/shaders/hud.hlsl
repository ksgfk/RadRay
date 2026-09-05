#include <core/platform.hlsli>
struct HudData { float4 Size; };
VK_BINDING(0,0) ConstantBuffer<HudData> HudFrame : register(b0,space0);
VK_BINDING(0,1) Texture2D<float4> FontTexture : register(t0,space1);
VK_BINDING(1,1) SamplerState FontSampler : register(s0,space1);
struct Input { float2 Position : POSITION; float2 UV : TEXCOORD0; float4 Color : COLOR0; };
struct Varying { float4 Position : SV_Position; float2 UV : TEXCOORD0; float4 Color : COLOR0; };
[shader("vertex")]
Varying VSMain(Input input) {
    Varying output;
    output.Position=float4(input.Position/HudFrame.Size.xy*float2(2,-2)+float2(-1,1),0,1);
    output.UV=input.UV; output.Color=input.Color; return output;
}
[shader("pixel")]
float4 PSMain(Varying input) : SV_Target0 { return float4(input.Color.rgb,input.Color.a*FontTexture.Sample(FontSampler,input.UV).a); }
