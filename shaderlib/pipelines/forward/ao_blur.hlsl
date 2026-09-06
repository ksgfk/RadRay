#include <pipelines/forward/effects.hlsli>
[shader("compute")] [numthreads(8, 8, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)Effects.Extent.xy)) return;
    float2 uv = uv_of(id.xy);
    float depth = InputB.SampleLevel(ClampSampler, uv, 0).x;
    float sum = 0, weights = 0;
    for (int i = -2; i <= 2; ++i) {
        float2 q = saturate(uv + i * Effects.Options.xy / Effects.Extent.xy);
        float w = exp(-abs(InputB.SampleLevel(ClampSampler, q, 0).x - depth) * 8) * (3 - abs(i));
        sum += InputA.SampleLevel(ClampSampler, q, 0).x * w; weights += w;
    }
    OutputScalar[id.xy] = sum / max(weights, 1e-6);
}

