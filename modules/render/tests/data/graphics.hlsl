#include <core/platform.hlsli>

struct DrawData {
    float4 Color;
    float Depth;
};

VK_PUSH_CONSTANT ConstantBuffer<DrawData> Draw : register(b0);

[shader("vertex")]
float4 FullscreenVS(uint vertex : SV_VertexID) : SV_Position {
    float2 corner = float2((vertex << 1) & 2, vertex & 2);
    return float4(corner * 2.0 - 1.0, Draw.Depth, 1.0);
}

[shader("pixel")]
float4 SolidPS() : SV_Target0 {
    return Draw.Color;
}
