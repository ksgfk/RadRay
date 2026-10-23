#include <core/platform.hlsli>
VK_BINDING(0, 0) Texture2DMS<float4> Source : register(t0);
VK_BINDING(1, 0) RWStructuredBuffer<float4> Output : register(u0);
[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    for (uint sample = 0; sample < 4; ++sample) Output[(id.y * 8 + id.x) * 4 + sample] = Source.Load(id.xy, sample);
}
