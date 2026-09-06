#include <pipelines/forward/surface.hlsli>
#include <pipelines/forward/local_light.hlsli>
#include <bsdf/principled.hlsli>
#include <core/frame.hlsli>
#include <shadow/cascade.hlsli>
struct ForwardPassData {
    float4x4 ShadowMatrix0, ShadowMatrix1, ShadowMatrix2, ShadowMatrix3;
    float4 ShadowSphere[4];
    float4 ShadowBias[4];
    float4 ShadowParams;
    float4 Extent; // width, height, tilesX, per-tile capacity
    uint LocalLightCount;
    uint UseTiles;
    uint UseAo;
    uint Transparent;
};
VK_BINDING(0, 3) ConstantBuffer<ForwardPassData> ForwardPass : register(b0, space3);
VK_BINDING(1, 3) Texture2DArray<float> ShadowMap : register(t0, space3);
VK_BINDING(2, 3) SamplerComparisonState ShadowSampler : register(s0, space3);
VK_BINDING(3, 3) StructuredBuffer<ForwardLocalLight> LocalLights : register(t1, space3);
VK_BINDING(4, 3) StructuredBuffer<uint2> TileHeaders : register(t2, space3);
VK_BINDING(5, 3) StructuredBuffer<uint> TileIndices : register(t3, space3);
VK_BINDING(6, 3) Texture2D<float> AmbientOcclusion : register(t4, space3);
VK_BINDING(7, 3) Texture2D<float4> OpaqueColor : register(t5, space3);
VK_BINDING(8, 3) SamplerState ScreenSampler : register(s1, space3);
[shader("vertex")] SurfaceVertexOutput VSMain(SurfaceVertexInput v) { return forward_surface_vertex(v); }
[shader("pixel")] float4 PSMain(SurfaceVertexOutput v) : SV_Target0 {
    float4 base = forward_surface_color(v.UV);
    if (ForwardMaterial.Transmission.y != 0) return float4(base.rgb * max(1, ForwardMaterial.Surface.w), base.a);
    float3 n = safe_normalize(v.Normal, float3(0, 1, 0));
    Frame3 frame = make_frame(n);
    float3 wi = frame_to_local(frame, safe_normalize(ForwardView.EyePosition.xyz - v.WorldPosition, n));
    PrincipledMaterial material = make_principled_material(base.rgb, saturate(ForwardMaterial.Surface.x), clamp(ForwardMaterial.Surface.y, .04, 1));
    float2 uv = v.Position.xy / ForwardPass.Extent.xy;
    float ao = ForwardPass.UseAo != 0 ? AmbientOcclusion.SampleLevel(ScreenSampler, uv, 0) : 1;
    float3 color = base.rgb * (.03 * ao + ForwardMaterial.Surface.w);
    CascadeShadow shadow;
    shadow.WorldToShadow[0] = ForwardPass.ShadowMatrix0; shadow.WorldToShadow[1] = ForwardPass.ShadowMatrix1;
    shadow.WorldToShadow[2] = ForwardPass.ShadowMatrix2; shadow.WorldToShadow[3] = ForwardPass.ShadowMatrix3;
    [unroll] for (uint c = 0; c < 4; ++c) { shadow.CascadeSphere[c] = ForwardPass.ShadowSphere[c]; shadow.CascadeBias[c] = ForwardPass.ShadowBias[c]; }
    shadow.Params = ForwardPass.ShadowParams;
    for (uint d = 0; d < ForwardView.DirectionalLightCount; ++d) {
        float3 l = directional_light_direction(ForwardView.DirectionalLights[d]);
        float visibility = d == 0 ? sample_cascade_shadow(ShadowMap, ShadowSampler, shadow, v.WorldPosition, n, l) : 1;
        color += eval_principled_reflection(material, wi, frame_to_local(frame, l)) * eval_directional_irradiance(ForwardView.DirectionalLights[d]) * visibility;
    }
    uint2 tile = (uint2)v.Position.xy / 16;
    uint index = tile.y * (uint)ForwardPass.Extent.z + tile.x;
    uint2 header = TileHeaders[index];
    bool allLights = ForwardPass.UseTiles == 0 || header.y != 0;
    uint count = allLights ? ForwardPass.LocalLightCount : header.x;
    for (uint i = 0; i < count; ++i) {
        uint lightIndex = allLights ? i : TileIndices[index * (uint)ForwardPass.Extent.w + i];
        ForwardLocalLight light = LocalLights[lightIndex];
        float3 toLight = light.PositionRadius.xyz - v.WorldPosition;
        float distanceSquared = dot(toLight, toLight);
        float3 l = safe_normalize(toLight, n);
        float range = saturate(1 - distanceSquared / max(1e-6, light.PositionRadius.w * light.PositionRadius.w));
        float cone = light.ColorType.w > .5 ? saturate((dot(-l, light.DirectionCosOuter.xyz) - light.DirectionCosOuter.w) * light.Cone.x) : 1;
        float3 irradiance = light.ColorType.rgb * (range * range * cone * cone / max(.01, distanceSquared));
        color += eval_principled_reflection(material, wi, frame_to_local(frame, l)) * irradiance;
    }
    if (ForwardPass.Transparent != 0 && ForwardMaterial.Transmission.x != 0) {
        float2 refracted = saturate(uv + n.xy * ForwardMaterial.Transmission.x / ForwardPass.Extent.xy);
        color = lerp(OpaqueColor.SampleLevel(ScreenSampler, refracted, 0).rgb * base.rgb, color, base.a);
    }
    return float4(color, base.a);
}
