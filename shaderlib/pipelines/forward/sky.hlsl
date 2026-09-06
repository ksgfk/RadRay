#include <pipelines/forward/effects.hlsli>
struct Varying { float4 Position : SV_Position; float2 UV : TEXCOORD0; };
[shader("vertex")] Varying VSMain(uint id : SV_VertexID) {
    Varying o; o.UV = float2((id << 1) & 2, id & 2);
    o.Position = float4(o.UV * float2(2, -2) + float2(-1, 1), 1, 1); return o;
}
[shader("pixel")] float4 PSMain(Varying v) : SV_Target0 {
    float2 clip = v.UV * float2(2, -2) + float2(-1, 1);
    float4 nearPoint = mul(Effects.InverseViewProjection, float4(clip, 0, 1));
    float4 farPoint = mul(Effects.InverseViewProjection, float4(clip, 1, 1));
    float3 direction = normalize(farPoint.xyz * nearPoint.w - nearPoint.xyz * farPoint.w);
    float elevation = direction.y;
    float3 color = lerp(float3(.22, .35, .44), float3(.025, .065, .14), smoothstep(0, .8, elevation));
    color = lerp(float3(.035, .05, .055), color, smoothstep(-.2, .02, elevation));
    color += float3(.8, .4, .12) * pow(saturate(1 - abs(elevation) * 5), 8);
    return float4(color, 1);
}
