#include <core/platform.hlsli>

VK_BINDING(6, 0) RWStructuredBuffer<uint> Output : register(u2);

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    Output[dispatchThreadId.x] = 0x12345678;
}
