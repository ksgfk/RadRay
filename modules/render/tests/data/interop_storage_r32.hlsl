#include <core/platform.hlsli>
struct PushData { uint Seed, Width, Height, Format; };
VK_PUSH_CONSTANT ConstantBuffer<PushData> Push : register(b0);
VK_BINDING(0, 0) VK_IMAGE_FORMAT("r32f") RWTexture2D<float> Target : register(u0);
float4 Pattern(uint2 p) {
    uint n = (Push.Seed + p.x * 3 + p.y * 5) & 15;
    if (Push.Format == 0) return float4(n * 17, (Push.Seed & 15) * 17, (p.x ^ p.y) & 255, 255) / 255.0;
    if (Push.Format == 1) return float4(1.0 + n / 32.0, 1.0 + (Push.Seed & 15) / 32.0, 1.0 + ((p.x ^ p.y) & 255) / 1024.0, 1);
    return float4(Push.Seed * 1024 + p.x + p.y * 512, 0, 0, 1);
}
[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x < Push.Width && id.y < Push.Height) Target[id.xy] = Pattern(id.xy).x;
}
