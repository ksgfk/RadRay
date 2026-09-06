#include <pipelines/forward/effects.hlsli>
VK_BINDING(12, 0) RWStructuredBuffer<float4> Particles : register(u4);
VK_BINDING(13, 0) RWStructuredBuffer<uint> Arguments : register(u5);
[shader("compute")] [numthreads(8, 8, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.y != 0 || id.x >= (uint)Effects.Options.y) return;
    float index = (float)id.x, time = Effects.Options.x;
    float angle = index * 2.39996323 + time * .15;
    float radius = 2 + frac(index * .61803) * 12;
    Particles[id.x] = float4(cos(angle) * radius, 1 + frac(index * .75487766) * 6 + .5 * sin(time + index), 5 + sin(angle) * radius, .035 + .035 * frac(index * .34));
    if (id.x == 0) { Arguments[0] = 6; Arguments[1] = (uint)Effects.Options.y; Arguments[2] = Arguments[3] = 0; }
}
