#include <core/platform.hlsli>

struct ObjectValues { column_major float4x4 LocalToWorld; };
struct ColorValues { float4 Color; };
struct ViewValues { float4 Data; };
struct DrawValues { uint Index; };

#if MIXED
VK_BINDING(0, 0) StructuredBuffer<ObjectValues> Objects : register(t0);
VK_BINDING(1, 0) StructuredBuffer<ColorValues> InstanceTint : register(t1);
VK_BINDING(2, 0) ConstantBuffer<ViewValues> ViewData : register(b0);
VK_BINDING(3, 0) StructuredBuffer<uint> VisibleSlots : register(t2);
VK_BINDING(4, 0) ConstantBuffer<ColorValues> Tint : register(b1);
VK_BINDING(5, 0) Texture2D<float4> Image : register(t3);
#define REUSE_ROOT "DescriptorTable(SRV(t0,numDescriptors=4),CBV(b0,numDescriptors=2)),DescriptorTable(Sampler(s0,space=3)),RootConstants(num32BitConstants=1,b0,space=4)"
#else
VK_BINDING(0, 0) StructuredBuffer<ObjectValues> Objects : register(t0, space0);
VK_BINDING(1, 0) StructuredBuffer<ColorValues> InstanceTint : register(t1, space0);
VK_BINDING(0, 1) ConstantBuffer<ViewValues> ViewData : register(b0, space1);
VK_BINDING(1, 1) StructuredBuffer<uint> VisibleSlots : register(t0, space1);
VK_BINDING(0, 2) ConstantBuffer<ColorValues> Tint : register(b0, space2);
VK_BINDING(1, 2) Texture2D<float4> Image : register(t0, space2);
#define REUSE_ROOT "DescriptorTable(SRV(t0,space=0,numDescriptors=2)),DescriptorTable(CBV(b0,space=1),SRV(t0,space=1)),DescriptorTable(CBV(b0,space=2),SRV(t0,space=2)),DescriptorTable(Sampler(s0,space=3)),RootConstants(num32BitConstants=1,b0,space=4)"
#endif
VK_BINDING(0, 3) SamplerState ImageSampler : register(s0, space3);
VK_PUSH_CONSTANT ConstantBuffer<DrawValues> DrawData : register(b0, space4);

struct Vertex {
    float4 Position : SV_Position;
    nointerpolation float4 InstanceColor : TEXCOORD0;
    nointerpolation float U : TEXCOORD1;
};
[RootSignature(REUSE_ROOT)]
[shader("vertex")]
Vertex VSMain(uint vertex : SV_VertexID) {
    const float2 positions[3] = {float2(-1,-1),float2(-1,3),float2(3,-1)};
    const uint slot = VisibleSlots[DrawData.Index];
    Vertex result;
    result.InstanceColor = InstanceTint[slot].Color;
    result.Position = mul(Objects[slot].LocalToWorld, float4(positions[vertex], ViewData.Data.x + 0.01 * result.InstanceColor.x, 1));
    result.U = ViewData.Data.y;
    return result;
}
[RootSignature(REUSE_ROOT)]
[shader("pixel")]
float4 PSMain(Vertex input) : SV_Target {
    return Tint.Color * input.InstanceColor * Image.Sample(ImageSampler, float2(input.U, 0.5));
}
