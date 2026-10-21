#include <core/platform.hlsli>

struct ObjectValues { column_major float4x4 LocalToWorld; };
struct ColorValues { float4 Color; };
struct ViewValues { float4 Data; };
struct DrawValues { uint Index; };
VK_BINDING(0, 0) StructuredBuffer<ObjectValues> Objects : register(t0, space0);
VK_BINDING(1, 0) StructuredBuffer<ColorValues> InstanceTint : register(t1, space0);
VK_BINDING(0, 1) ConstantBuffer<ViewValues> ViewData : register(b0, space1);
VK_BINDING(1, 1) StructuredBuffer<uint> VisibleSlots : register(t0, space1);
VK_PUSH_CONSTANT ConstantBuffer<DrawValues> DrawData : register(b0, space4);
#define REUSE_ROOT "DescriptorTable(SRV(t0,space=0,numDescriptors=2)),DescriptorTable(CBV(b0,space=1),SRV(t0,space=1)),RootConstants(num32BitConstants=1,b0,space=4)"

[RootSignature(REUSE_ROOT)]
[shader("vertex")]
float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    const float2 positions[3] = {float2(-1,-1),float2(-1,3),float2(3,-1)};
    const uint slot = VisibleSlots[DrawData.Index];
    return mul(Objects[slot].LocalToWorld, float4(positions[vertex], ViewData.Data.x + 0.01 * InstanceTint[slot].Color.x, 1));
}
