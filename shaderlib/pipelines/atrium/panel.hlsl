#include <core/platform.hlsli>
struct PanelData { float4x4 Transform; float4 Tint; };
VK_BINDING(0,0) ConstantBuffer<PanelData> PanelFrame : register(b0,space0);
VK_BINDING(0,1) Texture2D<float4> PanelTexture : register(t0,space1);
VK_BINDING(1,1) SamplerState PanelSampler : register(s0,space1);
struct Varying { float4 Position : SV_Position; float2 UV : TEXCOORD0; };
[shader("vertex")]
Varying VSMain(uint id : SV_VertexID) {
    const float2 corners[6]={float2(0,0),float2(1,1),float2(1,0),float2(0,0),float2(0,1),float2(1,1)};
    Varying output;
    output.UV=corners[id];
    output.Position=mul(PanelFrame.Transform,float4(output.UV*float2(1,-1)+float2(-.5,.5),0,1));
    return output;
}
[shader("pixel")]
float4 PSMain(Varying input) : SV_Target0 { return PanelTexture.Sample(PanelSampler,input.UV)*PanelFrame.Tint; }
