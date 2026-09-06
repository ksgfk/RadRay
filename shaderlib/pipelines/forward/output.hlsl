#include <pipelines/forward/effects.hlsli>
struct Fullscreen { float4 Position : SV_Position; float2 UV : TEXCOORD0; };
[shader("vertex")] Fullscreen VSMain(uint id : SV_VertexID) {
    Fullscreen o; o.UV = float2(id == 2 ? 2 : 0, id == 1 ? 2 : 0);
    o.Position = float4(o.UV * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
[shader("pixel")] float4 PSMain(Fullscreen v) : SV_Target0 {
    float3 value = sample_a(v.UV);
    if (Effects.DebugMode == 5 && Effects.Options.w > .5) value = srgb_to_linear(value);
    if (Effects.DebugMode == 0) value = tonemap_reinhard_luminance(max(0, value + InputB.SampleLevel(ClampSampler, v.UV, 0).rgb * Effects.Options.y) * Effects.Options.x);
    else if (Effects.DebugMode == 1) value = (1 - exp(-value.x * .04)).xxx;
    else if (Effects.DebugMode == 2) value = value * .5 + .5;
    else if (Effects.DebugMode == 3) value = float3(value.xy * 16 + .5, .5);
    if (Effects.Options.z > .5) value = linear_to_srgb(saturate(value));
    return float4(value, 1);
}
