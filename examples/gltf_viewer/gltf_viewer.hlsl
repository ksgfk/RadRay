// glTF viewer shader with a Mitsuba3-inspired Principled BRDF reflection path.
// Transmission/transparent response is intentionally rendered black in this first viewer pass.
#include "principled.hlsl"
#include "light.hlsl"
#include "shadow.hlsl"
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
    uint4 Counts; // x directional count, y point count, z spot count
};

VK_PUSH_CONSTANT ConstantBuffer<SceneConstants> gScene : register(b0, space0);
VK_BINDING(0, 0) StructuredBuffer<DirectionalLightGpu> gDirectionalLights : register(t0, space0);
VK_BINDING(1, 0) StructuredBuffer<PointLightGpu> gPointLights : register(t1, space0);
VK_BINDING(2, 0) ConstantBuffer<LightInfo> gLightInfo : register(b1, space0);
VK_BINDING(3, 0) ConstantBuffer<ShadowParam> gShadowParam : register(b2, space0);
VK_BINDING(4, 0) Texture2DArray<float> gShadowMap : register(t2, space0);
VK_BINDING(5, 0) SamplerComparisonState gShadowSampler : register(s0, space0);
VK_BINDING(6, 0) StructuredBuffer<SpotLightGpu> gSpotLights : register(t3, space0);
VK_BINDING(7, 0) ConstantBuffer<AdditionalShadowParam> gAdditionalShadowParam : register(b3, space0);
VK_BINDING(8, 0) Texture2DArray<float> gAdditionalShadowMap : register(t4, space0);
VK_BINDING(0, 1) ConstantBuffer<MaterialConstants> gMaterial : register(b0, space1);
VK_BINDING(1, 1) Texture2D<float4> gBaseColor : register(t0, space1);
VK_BINDING(2, 1) Texture2D<float4> gNormalMap : register(t1, space1);
VK_BINDING(3, 1) Texture2D<float4> gMetallicRoughness : register(t2, space1);
VK_BINDING(4, 1) Texture2D<float4> gOcclusion : register(t3, space1);
VK_BINDING(5, 1) Texture2D<float4> gEmissive : register(t4, space1);
VK_BINDING(6, 1) SamplerState gMaterialSampler : register(s0, space1);

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    float3 positionWS = mul(gScene.Model, float4(input.Position, 1.0)).xyz;
    float3 normalWS = mul(gScene.Model, float4(input.Normal, 0.0)).xyz;
    if (gScene.Debug.x == 6) {
        float3 dirToLight = normalize(gScene.CameraPosition.xyz);
        float depthBias = gScene.CameraPosition.w;
        float normalBias = asfloat(gScene.Debug.y);
        float3 biasedWS = apply_shadow_bias(positionWS, normalize(normalWS), dirToLight, depthBias, normalBias);
        output.Position = apply_shadow_clamping(mul(gScene.MVP, float4(biasedWS, 1.0)));
    } else {
        output.Position = mul(gScene.MVP, float4(input.Position, 1.0));
    }
    output.WorldPosition = positionWS;
    output.WorldNormal = normalWS;
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
    if (gScene.Debug.x == 7) {
        uint count = (uint)gShadowParam.Params.z;
        uint cascadeIndex = compute_cascade_index(gShadowParam, input.WorldPosition, count);
        float3 colors[4] = {
            float3(1.00f, 0.20f, 0.16f),
            float3(0.15f, 0.85f, 0.25f),
            float3(0.20f, 0.45f, 1.00f),
            float3(1.00f, 0.86f, 0.18f),
        };
        if (cascadeIndex >= count) {
            return float4(0.18f, 0.18f, 0.18f, 1.0f);
        }
        return float4(colors[min(cascadeIndex, 3u)], 1.0f);
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

    // 方向命名严格遵循 Mitsuba3 principled BSDF 约定(见 principled.hlsl 顶部注释):
    //   wi = 视线方向(指向相机)= Mitsuba 的 si.wi,对应 cos_theta_i
    //   wo = 光源方向(指向光源)= Mitsuba eval() 第二参数 wo,对应 cos_theta_o
    // 注意:这与“wi 指向光源”的常见教科书习惯相反,EvalPrincipledReflection 内部
    // 的余弦投影(漫反射乘 cos_theta_o、镜面只除 cos_theta_i)依赖该约定,切勿调换。
    Frame3 frame = MakeShadingFrame(n, input.WorldTangent);
    float3 wi = to_local(frame, viewDirWorld);

    if (wi.z <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    float3 Lo = float3(0.0f, 0.0f, 0.0f);
    uint dirCount = gLightInfo.Counts.x;
    for (uint i = 0; i < dirCount; ++i) {
        DirectionalLightGpu L = gDirectionalLights[i];
        float3 woW = -normalize(L.Direction.xyz);
        float3 wo = to_local(frame, woW);
        if (wo.z <= 0.0f) {
            continue;
        }
        float3 Li = eval_directional_irradiance(L);
        float shadow = 1.0f;
        if (i == 0) {
            shadow = sample_shadow_cascade(gShadowMap, gShadowSampler, gShadowParam, input.WorldPosition);
        }
        Lo += EvalPrincipledReflection(
            normalize(wi), normalize(wo), saturate(base.rgb), metallic, roughness,
            specular, specTint,
            anisotropic, sheen,
            sheenTint, flatness,
            clearcoat, clearcoatGloss,
            specTrans, eta) * Li * shadow;
    }

    uint ptCount = gLightInfo.Counts.y;
    for (uint j = 0; j < ptCount; ++j) {
        PointLightGpu L = gPointLights[j];
        float3 dW = L.Position.xyz - input.WorldPosition;
        float3 woW = normalize(dW);
        float3 wo = to_local(frame, woW);
        if (wo.z <= 0.0f) {
            continue;
        }
        float3 Li = eval_point_irradiance(L, input.WorldPosition);
        float shadow = sample_point_shadow(
            gAdditionalShadowMap, gShadowSampler, gAdditionalShadowParam,
            L.Intensity.w, L.Position.xyz, input.WorldPosition);
        Lo += EvalPrincipledReflection(
            normalize(wi), normalize(wo), saturate(base.rgb), metallic, roughness,
            specular, specTint,
            anisotropic, sheen,
            sheenTint, flatness,
            clearcoat, clearcoatGloss,
            specTrans, eta) * Li * shadow;
    }

    uint spotCount = gLightInfo.Counts.z;
    for (uint k = 0; k < spotCount; ++k) {
        SpotLightGpu L = gSpotLights[k];
        float3 dW = L.Position.xyz - input.WorldPosition;
        float3 woW = normalize(dW);
        float3 wo = to_local(frame, woW);
        if (wo.z <= 0.0f) {
            continue;
        }
        float3 Li = eval_spot_irradiance(L, input.WorldPosition);
        float shadow = sample_spot_shadow(
            gAdditionalShadowMap, gShadowSampler, gAdditionalShadowParam,
            L.Params.x, input.WorldPosition);
        Lo += EvalPrincipledReflection(
            normalize(wi), normalize(wo), saturate(base.rgb), metallic, roughness,
            specular, specTint,
            anisotropic, sheen,
            sheenTint, flatness,
            clearcoat, clearcoatGloss,
            specTrans, eta) * Li * shadow;
    }

    float3 color = Lo * occlusion + emissive;
    color = color / (color + 1.0f.xxx);
    color = linear_to_srgb(saturate(color));
    return float4(color, base.a);
}
