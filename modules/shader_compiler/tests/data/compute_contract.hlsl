#include <core/platform.hlsli>

#pragma radray_keyword_group DEBUG_OUTPUT "off" "on"

VK_BINDING(6, 0) RWStructuredBuffer<uint> Output : register(u0);

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    Output[dispatchThreadId.x] = 0x12345678;
}
