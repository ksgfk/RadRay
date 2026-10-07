#include <core/platform.hlsli>
struct ObjectData { column_major float4x4 LocalToWorld; };
struct ViewParameters { column_major float4x4 ViewProjection; };
struct DrawParameters { uint ObjectSlot; };
struct MaterialParameters { float4 Tint; };
VK_BINDING(0, 0) StructuredBuffer<ObjectData> Objects : register(t0);
VK_BINDING(1, 0) ConstantBuffer<MaterialParameters> MaterialData : register(b0);
VK_BINDING(0, 1) ConstantBuffer<ViewParameters> ViewData : register(b0, space1);
VK_BINDING(0, 2) Texture2D<float4> Albedo : register(t0, space2);
VK_BINDING(0, 3) SamplerState AlbedoSampler : register(s0, space3);
VK_PUSH_CONSTANT ConstantBuffer<DrawParameters> DrawData : register(b1);
#define MATERIAL_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), DescriptorTable(SRV(t0), CBV(b0)), DescriptorTable(CBV(b0,space=1)), DescriptorTable(SRV(t0,space=2)), DescriptorTable(Sampler(s0,space=3)), RootConstants(num32BitConstants=1,b1)"
struct VertexOutput { float4 Position : SV_Position; float3 Color : COLOR0; float2 UV : TEXCOORD1; };
[RootSignature(MATERIAL_RS)]
[shader("vertex")]
VertexOutput VSMain(VK_LOCATION(0) float3 position : POSITION, VK_LOCATION(3) float2 uv : TEXCOORD1) {
    VertexOutput result;
    result.Position = mul(ViewData.ViewProjection, mul(Objects[DrawData.ObjectSlot].LocalToWorld, float4(position, 1)));
    result.UV = uv;
    result.Color = float3(0.25, 0.55, 0.85) + position * 0.3;
    return result;
}
[RootSignature(MATERIAL_RS)]
[shader("pixel")]
float4 PSMain(VertexOutput input) : SV_Target0 { return float4(input.Color, 1) * MaterialData.Tint * Albedo.Sample(AlbedoSampler, input.UV); }
