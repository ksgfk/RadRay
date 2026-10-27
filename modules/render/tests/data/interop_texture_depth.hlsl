#include <core/platform.hlsli>
struct PushData { uint Seed, Width, Height, Depth, Mip, Layer, Samples, Reserved; };
VK_PUSH_CONSTANT ConstantBuffer<PushData> Push : register(b0);
[shader("vertex")]
float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    return float4(vertex == 2 ? 3 : -1, vertex == 1 ? 3 : -1, 0.5, 1);
}
[shader("pixel")]
float PSMain() : SV_Depth {
    return float(1 + (Push.Seed * 13 + Push.Mip * 7 + Push.Layer * 11) % 7) / 8.0;
}
