#include <pipelines/forward/effects.hlsli>
[shader("compute")] [numthreads(8, 8, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)Effects.Extent.xy)) return;
    // A one-pixel guard covers all subpixel camera jitter without losing boundary lights.
    float2 low = (float2(id.xy * 16) - 1) / Effects.Extent.zw;
    float2 high = (float2((id.xy + 1) * 16) + 1) / Effects.Extent.zw;
    float left = low.x * 2 - 1, right = high.x * 2 - 1;
    float top = 1 - low.y * 2, bottom = 1 - high.y * 2;
    float4 planes[6] = {
        mul(float4(1, 0, 0, -left), Effects.Projection), mul(float4(-1, 0, 0, right), Effects.Projection),
        mul(float4(0, 1, 0, -bottom), Effects.Projection), mul(float4(0, -1, 0, top), Effects.Projection),
        mul(float4(0, 0, 1, 0), Effects.Projection), mul(float4(0, 0, -1, 1), Effects.Projection)
    };
    uint tile = id.y * (uint)Effects.Extent.x + id.x, count = 0;
    for (uint lightIndex = 0; lightIndex < Effects.LocalLightCount; ++lightIndex) {
        ForwardLocalLight light = Lights[lightIndex];
        float3 p = mul(Effects.WorldToView, float4(light.PositionRadius.xyz, 1)).xyz;
        bool inside = true;
        [unroll] for (uint plane = 0; plane < 6; ++plane)
            inside = inside && dot(planes[plane], float4(p, 1)) >= -light.PositionRadius.w * length(planes[plane].xyz);
        if (inside) {
            if (count < Effects.TileCapacity) Indices[tile * Effects.TileCapacity + count] = lightIndex;
            ++count;
        }
    }
    Headers[tile] = uint2(min(count, Effects.TileCapacity), count > Effects.TileCapacity ? 1 : 0);
}
