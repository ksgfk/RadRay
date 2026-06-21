// glTF viewer shader with a Mitsuba3-inspired Principled BRDF reflection path.
// Transmission/transparent response is intentionally rendered black in this first viewer pass.
#include "principled.hlsl"
#include "light.hlsl"
struct VertexInput {
    float3 Position : POSITION0;
    float3 Normal : NORMAL0;
    float2 TexCoord : TEXCOORD0;
    float4 Tangent : TANGENT0;
};

struct VertexOutput {
    float4 Position : SV_Position;
    float3 WorldPosition : POSITION0;
    float3 WorldNormal : NORMAL0;
    float4 WorldTangent : TANGENT0;
    float2 TexCoord : TEXCOORD0;
};

struct SceneConstants {
    float4x4 MVP;
    float4x4 Model;
    float4 CameraPosition;
    uint4 Debug; // x: 0=principled, 1=normal, 2=uv, 3=white
};

// per-material 参数(set1=per-material)。字段布局与原来逐顶点插值的 5 个 float4 一致。
struct MaterialConstants {
    float4 BaseColorFactor;
    float4 EmissiveFactorAlphaCutoff;
    float4 Principled0; // x metallic, y roughness, z specular, w specular tint
    float4 Principled1; // x anisotropic, y sheen, z sheen tint, w flatness
    float4 Principled2; // x clearcoat, y clearcoat gloss, z specular transmission, w eta
};

struct LightInfo {
    uint4 Counts; // x directional count, y point count
};

VK_PUSH_CONSTANT ConstantBuffer<SceneConstants> gScene : register(b0, space0);
VK_BINDING(0, 0) StructuredBuffer<DirectionalLightGpu> gDirectionalLights : register(t0, space0);
VK_BINDING(1, 0) StructuredBuffer<PointLightGpu> gPointLights : register(t1, space0);
VK_BINDING(2, 0) ConstantBuffer<LightInfo> gLightInfo : register(b1, space0);
VK_BINDING(0, 1) ConstantBuffer<MaterialConstants> gMaterial : register(b0, space1);
VK_BINDING(1, 1) Texture2D<float4> gBaseColor : register(t0, space1);
VK_BINDING(2, 1) Texture2D<float4> gNormalMap : register(t1, space1);
VK_BINDING(3, 1) Texture2D<float4> gMetallicRoughness : register(t2, space1);
VK_BINDING(4, 1) Texture2D<float4> gOcclusion : register(t3, space1);
VK_BINDING(5, 1) Texture2D<float4> gEmissive : register(t4, space1);
VK_BINDING(6, 1) SamplerState gMaterialSampler : register(s0, space1);

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    output.Position = mul(gScene.MVP, float4(input.Position, 1.0));
    output.WorldPosition = mul(gScene.Model, float4(input.Position, 1.0)).xyz;
    output.WorldNormal = mul(gScene.Model, float4(input.Normal, 0.0)).xyz;
    output.WorldTangent = float4(mul(gScene.Model, float4(input.Tangent.xyz, 0.0)).xyz, input.Tangent.w);
    output.TexCoord = input.TexCoord;
    return output;
}

float3 SampleTangentNormal(float2 uv) {
    return gNormalMap.Sample(gMaterialSampler, uv).xyz * 2.0f - 1.0f;
}

Frame3 MakeShadingFrame(float3 normal, float4 tangent) {
    Frame3 frame = make_frame(normal);
    float3 t = tangent.xyz - normal * dot(normal, tangent.xyz);
    float tangentLen2 = dot(t, t);
    if (tangentLen2 > 1e-8f) {
        frame.s = t * rsqrt(tangentLen2);
        frame.t = normalize(cross(frame.n, frame.s) * tangent.w);
    }
    return frame;
}

float3 DecodeNormal(float2 uv, float3 n, float4 tangent) {
    float3 normal = normalize(n);
    float3 t = tangent.xyz - normal * dot(normal, tangent.xyz);
    float tangentLen2 = dot(t, t);
    if (tangentLen2 <= 1e-8f) {
        return normal;
    }
    t *= rsqrt(tangentLen2);
    float3 b = normalize(cross(normal, t) * tangent.w);
    float3 tangentNormal = normalize(SampleTangentNormal(uv));
    return normalize(t * tangentNormal.x + b * tangentNormal.y + normal * tangentNormal.z);
}

float4 PSMain(VertexOutput input) : SV_Target0 {
    if (gScene.Debug.x == 1) {
        float3 n = normalize(input.WorldNormal);
        return float4(n * 0.5f + 0.5f, 1.0f);
    }
    if (gScene.Debug.x == 2) {
        return float4(frac(input.TexCoord), 0.0f, 1.0f);
    }
    if (gScene.Debug.x == 3) {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }
    if (gScene.Debug.x == 4) {
        return float4(0.5f, 0.5f, 1.0f, 1.0f);
    }

    float3 viewDirWorld = normalize(gScene.CameraPosition.xyz - input.WorldPosition);
    float3 n = DecodeNormal(input.TexCoord, input.WorldNormal, input.WorldTangent);
    if (dot(n, viewDirWorld) < 0.0f) {
        n = -n;
    }
    if (gScene.Debug.x == 5) {
        return float4(n * 0.5f + 0.5f, 1.0f);
    }

    float4 base = saturate(gMaterial.BaseColorFactor * gBaseColor.Sample(gMaterialSampler, input.TexCoord));
    float3 emissive = max(gMaterial.EmissiveFactorAlphaCutoff.rgb, 0.0f.xxx) *
        gEmissive.Sample(gMaterialSampler, input.TexCoord).rgb;
    float3 mr = gMetallicRoughness.Sample(gMaterialSampler, input.TexCoord).rgb;
    float occlusion = gOcclusion.Sample(gMaterialSampler, input.TexCoord).r;
    float metallic = saturate(gMaterial.Principled0.x * mr.b);
    float roughness = saturate(gMaterial.Principled0.y * mr.g);
    float specular = saturate(gMaterial.Principled0.z);
    float specTint = saturate(gMaterial.Principled0.w);
    float anisotropic = saturate(gMaterial.Principled1.x);
    float sheen = saturate(gMaterial.Principled1.y);
    float sheenTint = saturate(gMaterial.Principled1.z);
    float flatness = saturate(gMaterial.Principled1.w);
    float clearcoat = saturate(gMaterial.Principled2.x);
    float clearcoatGloss = saturate(gMaterial.Principled2.y);
    float specTrans = saturate(gMaterial.Principled2.z);
    float eta = max(gMaterial.Principled2.w, 1.001f);
    roughness = max(roughness, 0.001f);

    Frame3 frame = MakeShadingFrame(n, input.WorldTangent);
    float3 wo = to_local(frame, viewDirWorld);

    if (wo.z <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    float3 Lo = float3(0.0f, 0.0f, 0.0f);
    uint dirCount = gLightInfo.Counts.x;
    for (uint i = 0; i < dirCount; ++i) {
        DirectionalLightGpu L = gDirectionalLights[i];
        float3 wiW = -normalize(L.Direction.xyz);
        float3 wi = to_local(frame, wiW);
        if (wi.z <= 0.0f) {
            continue;
        }
        float3 Li = eval_directional_irradiance(L);
        Lo += EvalPrincipledReflection(
            normalize(wi), normalize(wo), saturate(base.rgb), metallic, roughness,
            specular, specTint,
            anisotropic, sheen,
            sheenTint, flatness,
            clearcoat, clearcoatGloss,
            specTrans, eta) * Li;
    }

    uint ptCount = gLightInfo.Counts.y;
    for (uint j = 0; j < ptCount; ++j) {
        PointLightGpu L = gPointLights[j];
        float3 dW = L.Position.xyz - input.WorldPosition;
        float3 wiW = normalize(dW);
        float3 wi = to_local(frame, wiW);
        if (wi.z <= 0.0f) {
            continue;
        }
        float3 Li = eval_point_irradiance(L, input.WorldPosition);
        Lo += EvalPrincipledReflection(
            normalize(wi), normalize(wo), saturate(base.rgb), metallic, roughness,
            specular, specTint,
            anisotropic, sheen,
            sheenTint, flatness,
            clearcoat, clearcoatGloss,
            specTrans, eta) * Li;
    }

    float3 color = Lo * occlusion + emissive;
    color = color / (color + 1.0f.xxx);
    color = linear_to_srgb(saturate(color));
    return float4(color, base.a);
}
