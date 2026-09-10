#include <core/platform.hlsli>
#include <pipelines/forward/cbuffers.hlsli>

VK_BINDING(0, 0)
ConstantBuffer<Forward_ViewData> ForwardView : register(b0, space0);
VK_BINDING(0, 2)
ConstantBuffer<Forward_ObjectData> ForwardObject : register(b0, space2);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return mul(ForwardView.ViewProj, mul(ForwardObject.LocalToWorld, float4(position, 1.0f)));
}
