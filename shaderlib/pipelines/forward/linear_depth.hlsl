#include <pipelines/forward/effects.hlsli>
[shader("compute")] [numthreads(8, 8, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)Effects.Extent.xy)) return;
    OutputScalar[id.xy] = max(0, view_position(uv_of(id.xy), InputA.Load(int3(id.xy, 0)).x).z);
}

