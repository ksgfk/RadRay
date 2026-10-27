#include <core/platform.hlsli>
struct PushData { uint Seed, Width, Height, Format; };
VK_PUSH_CONSTANT ConstantBuffer<PushData> Push : register(b0);
float4 Pattern(uint2 p) {
    uint n = (Push.Seed + p.x * 3 + p.y * 5) & 15;
    if (Push.Format == 0) return float4(n * 17, (Push.Seed & 15) * 17, (p.x ^ p.y) & 255, 255) / 255.0;
    if (Push.Format == 1) return float4(1.0 + n / 32.0, 1.0 + (Push.Seed & 15) / 32.0, 1.0 + ((p.x ^ p.y) & 255) / 1024.0, 1);
    return float4(Push.Seed * 1024 + p.x + p.y * 512, 0, 0, 1);
}
[shader("vertex")]
float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    return float4(vertex == 2 ? 3.0 : -1.0, vertex == 1 ? 3.0 : -1.0, 0.5, 1.0);
}
[shader("pixel")]
float4 PSMain(float4 position : SV_Position) : SV_Target0 { return Pattern(uint2(position.xy)); }
