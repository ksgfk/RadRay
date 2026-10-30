#include <core/platform.hlsli>
VK_BINDING(0, 0) RWStructuredBuffer<uint> values : register(u0);
[shader("compute")]
[numthreads(8, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    values[id.x] = values[id.x] * 3 + 7;
}
