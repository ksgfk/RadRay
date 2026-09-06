#include <pipelines/forward/effects.hlsli>
[shader("compute")] [numthreads(8, 8, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)Effects.Extent.xy)) return;
    uint2 begin = id.xy * (uint2)Effects.Extent.zw / (uint2)Effects.Extent.xy;
    uint2 end = (id.xy + 1) * (uint2)Effects.Extent.zw / (uint2)Effects.Extent.xy;
    float depth = 3.402823466e+38;
    for (uint y = begin.y; y < end.y; ++y) for (uint x = begin.x; x < end.x; ++x)
        depth = min(depth, InputA.Load(int3(x, y, 0)).x);
    OutputScalar[id.xy] = depth;
}

