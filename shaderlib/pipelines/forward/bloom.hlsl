#include <pipelines/forward/effects.hlsli>
[shader("compute")] [numthreads(8, 8, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)Effects.Extent.xy)) return;
    float2 uv = uv_of(id.xy), texel = .5 / Effects.Extent.zw;
    float3 value = (sample_a(uv + texel) + sample_a(uv - texel) + sample_a(uv + texel * float2(1, -1)) + sample_a(uv + texel * float2(-1, 1))) * .25;
#if FORWARD_EFFECT == 5
    value *= saturate((max(value.r, max(value.g, value.b)) - 1) / max(1, max(value.r, max(value.g, value.b))));
#elif FORWARD_EFFECT == 7
    value += InputB.SampleLevel(ClampSampler, uv, 0).rgb;
#endif
    OutputColor[id.xy] = float4(value, 1);
}

