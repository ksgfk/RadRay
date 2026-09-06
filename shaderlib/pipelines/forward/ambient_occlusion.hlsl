#include <pipelines/forward/effects.hlsli>
[shader("compute")] [numthreads(8, 8, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)Effects.Extent.xy)) return;
    float2 uv = uv_of(id.xy);
    float center = InputA.SampleLevel(ClampSampler, uv, 0).x;
    float3 n = InputB.SampleLevel(ClampSampler, uv, 0).xyz;
    float radius = Effects.Options.x;
    float2 screenRadius = min(.08, radius / max(center, .1)) * float2(Effects.Extent.y / Effects.Extent.x, 1);
    float occlusion = 0;
    [unroll] for (uint i = 0; i < 8; ++i) {
        float angle = (i + .5) * .7853981634;
        float2 direction = float2(cos(angle), sin(angle));
        [unroll] for (uint ring = 1; ring <= 2; ++ring) {
            float2 sampleUv = uv + direction * screenRadius * (ring * .5);
            float neighbor = ring == 1 ? InputA.SampleLevel(ClampSampler, saturate(sampleUv), 0).x : InputC.SampleLevel(ClampSampler, saturate(sampleUv), 0).x;
            float delta = center - neighbor;
            occlusion += (all(sampleUv >= 0) && all(sampleUv <= 1)) ? saturate((delta - .02 * radius) / max(.01, radius)) * saturate(1 - delta / max(.01, 4 * radius)) : 0;
        }
    }
    OutputScalar[id.xy] = dot(n, n) < .1 ? 1 : saturate(1 - occlusion / 8);
}
