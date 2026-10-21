#include <core/platform.hlsli>

struct FirstRoot {
    float4x4 FirstTransform;
    float4 FirstTint;
};

struct SecondRoot {
    float4 SecondOffsets[4];
    float SecondWeight;
};

VK_BINDING(0, 0)
ConstantBuffer<FirstRoot> First : register(b0, space0);

VK_BINDING(0, 1)
ConstantBuffer<SecondRoot> Second : register(b0, space1);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return mul(First.FirstTransform, float4(position, 1.0));
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return First.FirstTint + Second.SecondOffsets[2] * Second.SecondWeight;
}
