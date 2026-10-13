#include <core/platform.hlsli>
struct ObjectData { column_major float4x4 LocalToWorld; };
struct ViewParameters { column_major float4x4 ViewProjection; };
struct DrawParameters { uint ObjectSlot; };
struct MaterialParameters { float4 Tint; };
VK_BINDING(0, 0) StructuredBuffer<ObjectData> Objects : register(t0);
VK_BINDING(1, 0) ConstantBuffer<ViewParameters> ViewData : register(b0);
VK_BINDING(0, 1) ConstantBuffer<MaterialParameters> MaterialData : register(b0, space1);
VK_PUSH_CONSTANT ConstantBuffer<DrawParameters> DrawData : register(b1);
#define PRESSURE_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), DescriptorTable(SRV(t0),CBV(b0)), DescriptorTable(CBV(b0,space=1)), RootConstants(num32BitConstants=1,b1)"
struct VertexOutput { float4 Position : SV_Position; };
[RootSignature(PRESSURE_RS)]
[shader("vertex")]
VertexOutput VSMain(VK_LOCATION(0) float3 position : POSITION) {
    VertexOutput result;
    result.Position = mul(ViewData.ViewProjection, mul(Objects[DrawData.ObjectSlot].LocalToWorld, float4(position, 1)));
    return result;
}
[RootSignature(PRESSURE_RS)]
[shader("pixel")]
float4 PSMain(VertexOutput input) : SV_Target0 { return MaterialData.Tint; }
