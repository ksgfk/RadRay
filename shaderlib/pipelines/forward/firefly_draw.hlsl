#include <pipelines/forward/effects.hlsli>
VK_BINDING(12, 0) StructuredBuffer<float4> Particles : register(t6);
struct Varying { float4 Position : SV_Position; float2 UV : TEXCOORD0; float Hue : TEXCOORD1; };
[shader("vertex")] Varying VSMain(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 corners[6] = {float2(-1, -1), float2(-1, 1), float2(1, -1), float2(1, -1), float2(-1, 1), float2(1, 1)};
    float4 particle = Particles[instance]; float2 corner = corners[vertex];
    float3 position = mul(Effects.WorldToView, float4(particle.xyz, 1)).xyz;
    position.xy += corner * particle.w;
    Varying o; o.Position = mul(Effects.Projection, float4(position, 1)); o.UV = corner; o.Hue = frac(instance * .618); return o;
}
[shader("pixel")] float4 PSMain(Varying v) : SV_Target0 {
    float intensity = pow(saturate(1 - dot(v.UV, v.UV)), 3) * 8;
    return float4(lerp(float3(.1, 1, .65), float3(1, .4, .08), v.Hue) * intensity, 0);
}
