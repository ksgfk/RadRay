#include <core/platform.hlsli>

struct SharedRoot {
    float4 Value;
};

VK_BINDING(0, 0)
ConstantBuffer<SharedRoot> First : register(b0, space0);

VK_BINDING(0, 1)
ConstantBuffer<SharedRoot> Second : register(b0, space1);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0) + First.Value;
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return First.Value + Second.Value;
}
