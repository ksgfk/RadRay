#include <core/platform.hlsli>
struct PushData { uint Seed, Width, Height, Depth, Mip, Layer, Samples, Reserved; };
VK_PUSH_CONSTANT ConstantBuffer<PushData> Push : register(b0);
[shader("vertex")]
float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    return float4(vertex == 2 ? 3 : -1, vertex == 1 ? 3 : -1, 0.5, 1);
}
[shader("pixel")]
VALUE_TYPE PSMain(float4 position : SV_Position) : SV_Target0 {
    uint n = Push.Seed * 13 + Push.Mip * 7 + Push.Layer * 11 + uint(position.y) * 3 + uint(position.x);
    return VALUE_TYPE(n & 1, (n >> 1) & 1, (n >> 2) & 1, (n >> 3) & 1);
}
