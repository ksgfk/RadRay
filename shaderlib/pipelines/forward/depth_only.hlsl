#include <core/platform.hlsli>

struct ForwardDepthViewData {
    float4x4 ViewProj;
};
struct ForwardDepthObjectData {
    float4x4 LocalToWorld;
};

VK_BINDING(0, 0)
ConstantBuffer<ForwardDepthViewData> ForwardView : register(b0, space0);
VK_BINDING(0, 2)
ConstantBuffer<ForwardDepthObjectData> ForwardObject : register(b0, space2);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return mul(ForwardView.ViewProj, mul(ForwardObject.LocalToWorld, float4(position, 1.0f)));
}
