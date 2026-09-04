#include <core/platform.hlsli>
#include <lighting/lights.hlsli>

struct ForwardViewData {
    float4x4 ViewProj;
    float4 EyePosition;
    uint DirectionalLightCount;
    uint PointLightCount;
    float2 LightCountPadding;
    DirectionalLight DirectionalLights[RADRAY_MAX_DIRECTIONAL_LIGHTS];
    PointLight PointLights[RADRAY_MAX_POINT_LIGHTS];
};
struct ForwardMaterialData {
    float4 BaseColor;
    float3 NormalBias;
    float Roughness;
    float2 Tint;
    float2 MaterialPadding;
    float4x4 MaterialTransform;
};
struct ForwardObjectData {
    float4x4 LocalToWorld;
};

VK_BINDING(0, 2) ConstantBuffer<ForwardViewData> ForwardView : register(b0, space4);
VK_BINDING(0, 5) ConstantBuffer<ForwardMaterialData> ForwardMaterial : register(b0, space7);
VK_BINDING(1, 5) Texture2D<float4> AlbedoTexture : register(t0, space7);
VK_BINDING(2, 5) SamplerState LinearSampler : register(s0, space7);
#if defined(MISSING_FORWARD_OBJECT)
VK_BINDING(0, 8) ConstantBuffer<ForwardObjectData> ObjectData : register(b0, space9);
#else
VK_BINDING(0, 8) ConstantBuffer<ForwardObjectData> ForwardObject : register(b0, space9);
#endif

struct VertexInput {
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
};
struct VertexOutput {
    float4 Position : SV_Position;
    float3 Normal : NORMAL0;
    float2 UV : TEXCOORD0;
};

[shader("vertex")]
VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
#if defined(MISSING_FORWARD_OBJECT)
    float4 world = mul(ObjectData.LocalToWorld, float4(input.Position, 1));
#else
    float4 world = mul(ForwardObject.LocalToWorld, float4(input.Position, 1));
#endif
    output.Position = mul(ForwardView.ViewProj, world);
    output.Normal = input.Normal;
    output.UV = input.UV;
    return output;
}

[shader("pixel")]
float4 PSMain(VertexOutput input) : SV_Target0 {
    return AlbedoTexture.Sample(LinearSampler, input.UV) * ForwardMaterial.BaseColor * abs(input.Normal.z);
}
