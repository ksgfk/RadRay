#define M_PI 3.141592653589793

float3 FresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float DistributionGGX(float3 N, float3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / (3.1415926 * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx1 = GeometrySchlickGGX(NdotV, roughness);
    float ggx2 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}


struct VertexInput {
    float3 position: POSITION;
    float3 normal: NORMAL;
    float2 uv0: TEXCOORD0;
};

struct V2P {
    float4 pos: SV_POSITION;
    float3 normal: NORMAL;
    float3 posW: POSITION;
    float2 uv0: TEXCOORD0;
};

struct PreObject {
    float4x4 mvp;
    float4x4 model;
    float3 cameraPos;
};

struct ObjectMaterial {
    float3 baseColor;
    float metallic;
    float roughness;
};

struct PointLight {
    float3 posW;
    float3 color;
    float intensity;
};

ConstantBuffer<PreObject> g_PreObject: register(b0);
ConstantBuffer<ObjectMaterial> g_Material: register(b1);
ConstantBuffer<PointLight> g_PointLight: register(b2);

#ifdef BASE_COLOR_USE_TEXTURE
Texture2D<float4> g_BaseColor: register(t0);
SamplerState g_BaseColorSampler: register(s0);
#endif

#ifdef NORMAL_MAP_ENABLE
Texture2D<float4> g_NormalMap: register(t1);
SamplerState g_NormalMapSampler: register(s1);
#endif

V2P VSMain(VertexInput v) {
    V2P v2p;
    v2p.pos = mul(g_PreObject.mvp, float4(v.position, 1));
    v2p.normal = normalize(mul((float3x3)g_PreObject.model, v.normal));
    v2p.posW = mul(g_PreObject.model, float4(v.position, 1)).xyz;
    v2p.uv0 = v.uv0;
    return v2p;
}

float4 PSMain(V2P v2p) : SV_Target {
    float3 cameraPos = g_PreObject.cameraPos;
    float3 shadePos = v2p.posW;
    float3 lightPos = g_PointLight.posW;

#ifdef BASE_COLOR_USE_TEXTURE
    float3 baseColor = g_BaseColor.Sample(g_BaseColorSampler, v2p.uv0).rgb;
    baseColor = pow(baseColor, (float3)2.2);
#else
    float3 baseColor = g_Material.baseColor;
#endif
    float metallic = g_Material.metallic;
    float roughness = clamp(g_Material.roughness, 0.04, 1.0);

    float3 N = normalize(v2p.normal);
    float3 V = normalize(cameraPos - shadePos);
    float3 L = normalize(lightPos - shadePos);
    float3 H = normalize(V + L);

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metallic);
    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    float3 specular = (NDF * G * F) / (4.0 * NdotV * NdotL + 0.0001);

    float3 kS = F;
    float3 kD = 1.0 - kS;
    kD *= 1.0 - metallic;

    float3 diffuse = kD * baseColor / M_PI;

    float3 Le = (diffuse + specular) * NdotL;

    float Ldis = length(lightPos - shadePos);
    float3 Li = g_PointLight.color * (float3)g_PointLight.intensity / (float3)(Ldis * Ldis);
    Le *= Li;

    Le = pow(Le, (float3)(1.0 / 2.2));

    return float4(Le, 1);
}
