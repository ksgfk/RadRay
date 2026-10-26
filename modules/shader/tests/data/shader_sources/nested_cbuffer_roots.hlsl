#include <core/platform.hlsli>

struct InnerRoot {
    float4 InnerValue;
};

struct OuterRoot {
    InnerRoot Nested;
};

VK_BINDING(0, 0)
ConstantBuffer<InnerRoot> Inner : register(b0, space0);

VK_BINDING(0, 1)
ConstantBuffer<OuterRoot> Outer : register(b0, space1);

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0) + Inner.InnerValue;
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return Outer.Nested.InnerValue;
}
