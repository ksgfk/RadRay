#include <pipelines/forward/effects.hlsli>
[shader("compute")] [numthreads(8, 8, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)Effects.Extent.xy)) return;
    float2 uv = uv_of(id.xy);
    float4 motion = InputC.Load(int3(id.xy, 0));
    float depth = InputD.Load(int3(id.xy, 0)).x;
    float2 previousUv = uv - motion.xy;
    bool valid = Effects.HistoryValid != 0 && motion.w > .5;
    if (depth >= .999999) {
        float4 world = mul(Effects.InverseViewProjection, float4(uv * float2(2, -2) + float2(-1, 1), 1, 1));
        float3 direction = world.xyz / world.w - Effects.Eye.xyz;
        float4 oldClip = mul(Effects.PreviousViewProjection, float4(direction, 0));
        previousUv = oldClip.w > 1e-6 ? oldClip.xy / oldClip.w * float2(.5, -.5) + .5 : uv;
        motion.z = 1;
        valid = Effects.HistoryValid != 0 && oldClip.w > 0;
    }
    valid = valid && all(previousUv > 0) && all(previousUv < 1);
    float oldDepth = InputE.SampleLevel(ClampSampler, saturate(previousUv), 0).x;
    valid = valid && abs(oldDepth - motion.z) < max(.0005, .003 * (1 - depth));
    float3 lo = sample_a(uv), hi = lo;
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        float3 value = sample_a(uv + float2(x, y) / Effects.Extent.xy);
        lo = min(lo, value); hi = max(hi, value);
    }
    float3 history = clamp(InputB.SampleLevel(ClampSampler, saturate(previousUv), 0).rgb, lo, hi);
    OutputColor[id.xy] = float4(lerp(sample_a(uv), history, valid ? .9 : 0), 1);
    OutputScalar[id.xy] = depth;
}
