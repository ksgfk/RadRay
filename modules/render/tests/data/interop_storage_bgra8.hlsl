#include <core/platform.hlsli>
struct PushData { uint Seed, Width, Height, Format; };
VK_PUSH_CONSTANT ConstantBuffer<PushData> Push : register(b0);
// SPIR-V uses Unknown via -fspv-use-unknown-image-format; BGRA8 has no typed image format.
VK_BINDING(0, 0) RWTexture2D<float4> Target : register(u0);
[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Push.Width || id.y >= Push.Height) return;
    uint n = (Push.Seed + id.x * 3 + id.y * 5) & 15;
    Target[id.xy] = float4(n * 17, (Push.Seed & 15) * 17, (id.x ^ id.y) & 255, 255) / 255.0;
}
