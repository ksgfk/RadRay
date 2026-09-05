#include <core/color.hlsli>
#include <core/math.hlsli>
#include <pipelines/forward/bindings.hlsli>

#pragma radray_keyword_group QUALITY "low" "high"

// A keyword group expands to a bare token, so it has to be pasted onto a prefix before
// it can be compared. The indirection through RADRAY_FORWARD_CAT is what lets QUALITY
// expand before the paste happens.
#define RADRAY_FORWARD_CAT_(a, b) a##b
#define RADRAY_FORWARD_CAT(a, b) RADRAY_FORWARD_CAT_(a, b)
#define RADRAY_FORWARD_QUALITY_low 0
#define RADRAY_FORWARD_QUALITY_high 1
#define RADRAY_FORWARD_QUALITY RADRAY_FORWARD_CAT(RADRAY_FORWARD_QUALITY_, QUALITY)

struct ForwardVertexInput {
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
};

struct ForwardVertexOutput {
    float4 Position : SV_Position;
    float3 PositionWorld : POSITION0;
    float3 NormalWorld : NORMAL0;
    float2 UV : TEXCOORD0;
};

[shader("vertex")]
ForwardVertexOutput VSMain(ForwardVertexInput input) {
    ForwardVertexOutput output;
    const float4 positionWorld = mul(ForwardObject.LocalToWorld, float4(input.Position, 1.0f));
    output.Position = mul(ForwardView.ViewProj, positionWorld);
    output.PositionWorld = positionWorld.xyz;
    output.NormalWorld = safe_normalize(
        mul((float3x3)ForwardObject.NormalToWorld, input.Normal),
        float3(0.0f, 1.0f, 0.0f));
    output.UV = input.UV;
    return output;
}

[shader("pixel")]
float4 PSMain(ForwardVertexOutput input) : SV_Target0 {
    const float3 normal = safe_normalize(input.NormalWorld, float3(0.0f, 1.0f, 0.0f));
    float3 irradiance = float3(0.03f, 0.03f, 0.03f);
    for (uint index = 0; index < ForwardView.DirectionalLightCount; ++index) {
        const float3 lightDirection = directional_light_direction(ForwardView.DirectionalLights[index]);
        irradiance += eval_directional_irradiance(ForwardView.DirectionalLights[index]) *
                      saturate(dot(normal, lightDirection));
    }
    for (uint index = 0; index < ForwardView.PointLightCount; ++index) {
        const float3 toLight = ForwardView.PointLights[index].Position.xyz - input.PositionWorld;
        const float radius = ForwardView.PointLights[index].Position.w;
        const float distanceSquared = dot(toLight, toLight);
        const float rangeWeight = saturate(1.0f - distanceSquared * safe_rcp(radius * radius));
#if RADRAY_FORWARD_QUALITY == RADRAY_FORWARD_QUALITY_high
        // Squared window: smooth falloff to zero at the light radius.
        const float rangeWindow = rangeWeight * rangeWeight;
#else
        // Linear window: one multiply cheaper, visibly harder edge at the radius.
        const float rangeWindow = rangeWeight;
#endif
        irradiance += eval_point_irradiance(ForwardView.PointLights[index], input.PositionWorld) *
                      saturate(dot(normal, safe_normalize(toLight, normal))) * rangeWindow;
    }
    const float3 albedo = AlbedoTexture.Sample(LinearSampler, input.UV).rgb * ForwardMaterial.BaseColor.rgb;
    const float3 color = albedo * irradiance * RADRAY_INV_PI;
    return float4(linear_to_srgb(tonemap_reinhard_luminance(color)), ForwardMaterial.BaseColor.a);
}
