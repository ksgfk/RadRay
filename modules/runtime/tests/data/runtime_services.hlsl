#include <core/platform.hlsli>

VK_BINDING(0, 0)
RWStructuredBuffer<uint> Output : register(u0);

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID) {
    Output[0] = 0xc0de1234;
}
