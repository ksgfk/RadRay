#include "common.hlsl"

struct VertexInput {
    float3 Position : POSITION0;
};

struct VertexOutput {
    float4 Position : SV_Position;
};

struct PerObject {
    float4x4 ObjectToWorld;
};

struct ViewConstants {
    float4x4 ViewProj;
};

VK_BINDING(1, 0) ConstantBuffer<PerObject> gPerObject : register(b1, space0);
VK_BINDING(0, 1) ConstantBuffer<ViewConstants> gView : register(b0, space1);

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    float4 worldPosition = mul(gPerObject.ObjectToWorld, float4(input.Position, 1.0));
    output.Position = mul(gView.ViewProj, worldPosition);
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target0 {
    (void)input;
    return float4(1.0, 0.0, 1.0, 1.0);
}
