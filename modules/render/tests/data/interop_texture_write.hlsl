#include <core/platform.hlsli>
struct PushData { uint Seed, Width, Height, Depth, Mip, Layer, Samples, Reserved; };
VK_PUSH_CONSTANT ConstantBuffer<PushData> Push : register(b0);
#if DIMENSION == 1
VK_BINDING(0, 0) RWTexture1DArray<VALUE_TYPE> Target : register(u0);
#elif DIMENSION == 3
VK_BINDING(0, 0) RWTexture3D<VALUE_TYPE> Target : register(u0);
#else
VK_BINDING(0, 0) RWTexture2DArray<VALUE_TYPE> Target : register(u0);
#endif
uint4 Pattern(uint3 p) {
    uint n = Push.Seed * 13 + Push.Mip * 7 + Push.Layer * 11 + p.z * 5 + p.y * 3 + p.x;
    return uint4(n & 1, (n >> 1) & 1, (n >> 2) & 1, (n >> 3) & 1);
}
[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Push.Width || id.y >= Push.Height || id.z >= Push.Depth) return;
    VALUE_TYPE value = VALUE_TYPE(Pattern(id));
    if (Push.Reserved != 0) value.xyz = (value.xyz * 128.0 + 64.0) / 255.0;
#if DIMENSION == 1
    Target[uint2(id.x, 0)] = value;
#elif DIMENSION == 3
    Target[id] = value;
#else
    Target[uint3(id.xy, 0)] = value;
#endif
}
