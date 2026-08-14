#include <core/platform.hlsli>
#include <core/color.hlsli>
#include <lighting/lights.hlsli>

struct FrameData {
    float4x4 ViewProj;
    float4x4 Model;
    float4 Albedo;
};

VK_BINDING(0, 0)
ConstantBuffer<FrameData> Frame : register(b0);

VK_BINDING(1, 0)
ConstantBuffer<DirectionalLight> Light : register(b1);

struct VSInput {
    float3 Position : POSITION;
    float3 Normal : NORMAL;
};

struct VSOutput {
    float4 Position : SV_Position;
    float3 Normal : NORMAL;
};

[shader("vertex")]
VSOutput VSMain(VSInput input) {
    VSOutput output;
    const float4 worldPosition = mul(Frame.Model, float4(input.Position, 1.0f));
    output.Position = mul(Frame.ViewProj, worldPosition);
    output.Normal = normalize(mul((float3x3)Frame.Model, input.Normal));
    return output;
}

[shader("pixel")]
float4 PSMain(VSOutput input) : SV_Target0 {
    const float3 normal = normalize(input.Normal);
    const float3 lightDirection = directional_light_direction(Light);
    const float3 irradiance = eval_directional_irradiance(Light);
    const float diffuse = saturate(dot(normal, lightDirection));
    const float3 linearColor = diffuse * Frame.Albedo.rgb * irradiance * RADRAY_INV_PI;
    return float4(linear_to_srgb(linearColor), 1.0f);
}
