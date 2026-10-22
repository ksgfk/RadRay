#include <core/platform.hlsli>

VK_BINDING(3, 0) StructuredBuffer<uint> ViewData : register(t1);
VK_BINDING(5, 1) StructuredBuffer<uint> MaterialData : register(t4);
VK_BINDING(2, 2) StructuredBuffer<uint> ObjectData : register(t8);
VK_BINDING(7, 2) RWStructuredBuffer<uint> Output : register(u2);

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain() {
    Output[0] = ViewData[0] * 10000 + MaterialData[0] * 100 + ObjectData[0];
}
