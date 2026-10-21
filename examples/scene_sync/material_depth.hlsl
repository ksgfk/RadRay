#include <core/platform.hlsli>
struct ObjectData { column_major float4x4 LocalToWorld; };
struct ViewParameters { column_major float4x4 ViewProjection; };
struct DrawParameters { uint ObjectSlot; };
VK_BINDING(0, 0) StructuredBuffer<ObjectData> Objects : register(t0);
VK_BINDING(0, 1) ConstantBuffer<ViewParameters> ViewData : register(b0, space1);
VK_PUSH_CONSTANT ConstantBuffer<DrawParameters> DrawData : register(b1);
#define DEPTH_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), DescriptorTable(SRV(t0)), DescriptorTable(CBV(b0,space=1)), RootConstants(num32BitConstants=1,b1)"
[RootSignature(DEPTH_RS)]
[shader("vertex")]
float4 VSMain(VK_LOCATION(0) float3 position : POSITION) : SV_Position {
    return mul(ViewData.ViewProjection, mul(Objects[DrawData.ObjectSlot].LocalToWorld, float4(position, 1)));
}
