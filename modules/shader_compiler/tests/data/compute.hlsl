#include <core/platform.hlsli>

#pragma radray_keyword_group COMPUTE_MODE "clear" "stamp"

VK_BINDING(6, 2)
RWStructuredBuffer<uint> Output : register(u0);

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    Output[dispatchThreadId.x] = 0xc0de1234;
}
