#include <core/platform.hlsli>
#include <core/math.hlsli>
#include <core/color.hlsli>
#include <lighting/lights.hlsli>

struct ViewData {
    float4x4 ViewProj;
    float4 EyePosition;
    uint DirectionalLightCount;
    uint PointLightCount;
    float2 Padding;
    DirectionalLight DirectionalLights[RADRAY_MAX_DIRECTIONAL_LIGHTS];
    PointLight PointLights[RADRAY_MAX_POINT_LIGHTS];
};
struct MaterialData { float4 BaseColor; float4 Surface; };
struct ObjectData { float4x4 LocalToWorld; };
VK_BINDING(0, 0) ConstantBuffer<ViewData> ForwardView : register(b0, space0);
VK_BINDING(0, 1) ConstantBuffer<MaterialData> ForwardMaterial : register(b0, space1);
VK_BINDING(1, 1) Texture2D<float4> AlbedoTexture : register(t0, space1);
VK_BINDING(2, 1) SamplerState LinearSampler : register(s0, space1);
VK_BINDING(0, 2) ConstantBuffer<ObjectData> ForwardObject : register(b0, space2);
struct Input { float3 Position : POSITION; float3 Normal : NORMAL; float2 UV : TEXCOORD0; };
struct Varying { float4 Position : SV_Position; float3 World : POSITION0; float3 Normal : NORMAL0; float2 UV : TEXCOORD0; };
[shader("vertex")]
Varying VSMain(Input input) {
    Varying output;
    float4 world = mul(ForwardObject.LocalToWorld, float4(input.Position, 1));
    output.Position = mul(ForwardView.ViewProj, world);
    output.World = world.xyz;
    // Instance scales are positive; cofactors preserve normals under nonuniform scaling.
    float3x3 m = (float3x3)ForwardObject.LocalToWorld;
    float3x3 cof = float3x3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]));
    output.Normal = safe_normalize(mul(cof, input.Normal), float3(0, 1, 0));
    output.UV = input.UV;
    return output;
}
[shader("pixel")]
float4 PSMain(Varying input) : SV_Target0 {
    float3 n = normalize(input.Normal);
    float3 v = safe_normalize(ForwardView.EyePosition.xyz - input.World, float3(0, 0, -1));
    float4 base = AlbedoTexture.Sample(LinearSampler, float2(input.UV.x, 1-input.UV.y) * ForwardMaterial.Surface.y) * ForwardMaterial.BaseColor;
    if (ForwardMaterial.Surface.w>.5) return float4(base.rgb,base.a);
    float3 light = lerp(float3(.055,.10,.14), float3(.32,.39,.43), n.y*.5+.5);
    float3 specular = 0;
    float gloss = ForwardMaterial.Surface.z;
    for (uint i=0; i<ForwardView.DirectionalLightCount; ++i) {
        float3 l = directional_light_direction(ForwardView.DirectionalLights[i]);
        float3 e = eval_directional_irradiance(ForwardView.DirectionalLights[i]);
        light += e * saturate(dot(n,l)) * .31831;
        specular += e * pow(saturate(dot(n,safe_normalize(l+v,n))), lerp(8,96,gloss)) * gloss * .2;
    }
    for (uint j=0; j<ForwardView.PointLightCount; ++j) {
        PointLight p = ForwardView.PointLights[j];
        float3 delta = p.Position.xyz-input.World;
        float radius = p.Position.w;
        float window = saturate(1-dot(delta,delta)/(radius*radius));
        float3 l = safe_normalize(delta,n);
        float3 e = eval_point_irradiance(p,input.World)*window*window;
        light += e * saturate(dot(n,l)) * .31831;
        specular += e * pow(saturate(dot(n,safe_normalize(l+v,n))),lerp(8,80,gloss)) * gloss * .28;
    }
    float emission = ForwardMaterial.Surface.x;
    float rim = pow(1-saturate(abs(dot(n,v))),3);
    float3 color = base.rgb * light + specular;
    color += base.rgb * emission * (1+.3*rim);
    float fog = 1-exp(-length(ForwardView.EyePosition.xyz-input.World)*.004);
    color = lerp(color,float3(.055,.105,.14),fog);
    float alpha = base.a;
    if (alpha < .99) { color += base.rgb * rim * .45; alpha = saturate(alpha + .25*rim); }
    return float4(linear_to_srgb(tonemap_reinhard_luminance(color)), alpha);
}
