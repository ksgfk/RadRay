#define M_PI (3.14159265359)
#define M_EPSILON (0.00001)

struct ShadingData
{
    float3 hitPos;
    float3 n;
    float3 ng;
    float3 t;
    float3 b;
    float3 wo;
    float3 wi;
};

struct MaterialData
{
    float3 albedo;
    float metallic;
  
    float3 specularTint;
    float specular;

    float roughness;
    float anisotropy;
    float ior;
};

struct DSPBRData
{
    float3 albedo;
    float metallic;
    float2 alpha;
    float specular;
    float ior;
    float f0;
    float3 specular_f0;
    float3 specular_f90;
};

float2 roughness_conversion(float roughness, float anisotropy)
{
    float2 a = float2(roughness * (1.0f + anisotropy), roughness * (1.0f - anisotropy));
    return max(a * a, (float2)0.0001);
}

DSPBRData get_dspbr_data(in MaterialData matData)
{
    DSPBRData dspbr;
    dspbr.albedo = matData.albedo;
    dspbr.metallic = matData.metallic;
    float roughness = matData.roughness;
    dspbr.alpha = roughness_conversion(roughness, matData.anisotropy);
    dspbr.specular = matData.specular;
    dspbr.ior = matData.ior;
    dspbr.f0 = ((dspbr.ior - 1.0f) / (dspbr.ior + 1.0f)) * ((dspbr.ior - 1.0f) / (dspbr.ior + 1.0f));
    dspbr.specular_f0 = lerp(dspbr.specular * dspbr.f0 * matData.specularTint, matData.albedo, matData.metallic);
    dspbr.specular_f90 = (float3)lerp(dspbr.specular, 1.0f, dspbr.metallic);
    return dspbr;
}

//----------------------------------------------------------------------------------------------

float pow5(float x)
{
    float x2 = x * x;
    return x2 * x2 * x;
}

float sqr(float x)
{
    return x * x;
}

float sign(float v)
{
    return v >= 0 ? 1.0 : -1.0;
}

float mul_sign(float v1, float v2)
{
    return v2 >= 0 ? v1 : -v1;
}

float safe_sqrt(float v)
{
    return sqrt(max(v, 0.0));
}

struct Frame
{
    float3 s;
    float3 t;
    float3 n;
};

void coordinate_system(in float3 n, out float3 s, out float3 t)
{
    float sig = sign(n.z);
    float a = -1.0 / (sig + n.z);
    float b = n.x * n.y * a;
    s = float3(mul_sign(sqr(n.x) * a, n.z) + 1.0f, mul_sign(b, n.z), mul_sign(n.x, -n.z));
    t = float3(b, sig + sqr(n.y) * a, -n.y);
}

float3 to_world(in Frame f, in float3 v)
{
    return v.x * f.s + v.y * f.t + v.z * f.n;
}

float3 to_local(in Frame f, in float3 v)
{
    return float3(dot(v, f.s), dot(v, f.t), dot(v, f.n));
}

float cos_theta(float3 v) { return v.z; }
float cos_theta_2(float3 v) { return sqr(v.z); }
float sin_theta_2(float3 v) { return sqr(v.x) + sqr(v.y); }
float sin_theta(float3 v) { return safe_sqrt(sin_theta_2(v)); }
float tan_theta(float3 v)
{
    float temp = -v.z * v.z + 1.0;
    return safe_sqrt(temp) / v.z;
}
float tan_theta_2(float3 v)
{
    float temp = -v.z * v.z + 1.0;
    return max(temp, 0.0) / sqr(v.z);
}
float sin_phi(float3 v)
{
    float sinTheta2 = sin_theta_2(v);
    float invSinTheta = 1 / sqrt(sinTheta2);
    return abs(sinTheta2) <= 4.0 * M_EPSILON ? 0.0 : clamp(v.y * invSinTheta, -1.0, 1.0);
}
float cos_phi(float3 v)
{
    float sinTheta2 = sin_theta_2(v);
    float invSinTheta = 1 / sqrt(sinTheta2);
    return abs(sinTheta2) <= 4.0 * M_EPSILON ? 1.0 : clamp(v.x * invSinTheta, -1.0, 1.0);
}
float sin_phi_2(float3 v)
{
    float sinTheta2 = sin_theta_2(v);
    return abs(sinTheta2) <= 4.0 * M_EPSILON ? 0.0 : clamp(sqr(v.y) / sinTheta2, -1.0, 1.0);
}
float cos_phi_2(float3 v)
{
    float sinTheta2 = sin_theta_2(v);
    return abs(sinTheta2) <= 4.0 * M_EPSILON ? 1.0 : clamp(sqr(v.x) / sinTheta2, -1.0, 1.0);
}
float abs_cos_theta(float3 v) { return abs(v.z); }

float3 fresnel_schlick(float3 f0, float3 f90, float cosThetaI)
{
    return f0 + (f90 - f0) * pow5(abs(1.0 - cosThetaI));
}

float ggx_smith_g1(float2 alpha, float3 v, float3 wh)
{
    float xyAlpha2 = sqr(alpha.x * v.x) + sqr(alpha.y * v.y);
    float tanThetaAlpha2 = xyAlpha2 / sqr(v.z);
    float result = 2 / (1 + sqrt(1 + tanThetaAlpha2));
    if (xyAlpha2 == 0)
    {
        result = 1;
    }
    if (dot(v, wh) * v.z <= 0.0)
    {
        result = 0.0;
    }
    return result;
}

float ggx_smith_g(float2 alpha, float3 wi, float3 wo, float3 wh)
{
    return ggx_smith_g1(alpha, wi, wh) * ggx_smith_g1(alpha, wo, wh);
}

float ggx_d(float2 alpha, float3 m)
{
    float alphaUV = alpha.x * alpha.y;
    float cosTheta = cos_theta(m);
    float result = 1.0 / (M_PI * alphaUV * sqr(sqr(m.x / alpha.x) + sqr(m.y / alpha.y) + sqr(m.z)));
    return result * cosTheta > 1e-20f ? result : 0;
}

float3 eval_brdf_microfacet_ggx(float2 alpha, float3 wi, float3 wo, float3 f0, float3 f90)
{
    if (cos_theta(wi) <= 0 || cos_theta(wo) <= 0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    float3 wh = normalize(wi + wo);
    float3 F = fresnel_schlick(f0, f90, dot(wi, wh));
    float D = ggx_d(alpha, wh);
    float G = ggx_smith_g(alpha, wi, wo, wh);
    return (F * D * G) / (4.0 * cos_theta(wi) * cos_theta(wo)) * cos_theta(wo);
}

//----------------------------------------------------------------------------------------------

struct PreObjectData
{
    float4x4 model;
    float4x4 mvp;
    float4x4 modelInv;
};

struct PreCameraData
{
    float4x4 view;
    float4x4 proj;
    float4x4 viewProj;
    float3 posW;
};

struct VS_INPUT
{
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float3 nor : NORMAL0;
    [[vk::location(2)]] float2 uv  : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    [[vk::location(0)]] float3 posW : POSITION;
    [[vk::location(1)]] float3 norW : NORMAL0;
    [[vk::location(2)]] float2 uv  : TEXCOORD0;
};

[[vk::push_constant]] ConstantBuffer<PreObjectData> _Obj : register(b0);
[[vk::binding(0, 0)]] ConstantBuffer<MaterialData> _Mat : register(b1);
[[vk::binding(1, 0)]] ConstantBuffer<PreCameraData> _Camera : register(b2);

PS_INPUT VSMain(VS_INPUT vsIn)
{
    PS_INPUT psIn;
    psIn.pos = mul(_Obj.mvp, float4(vsIn.pos, 1.0));
    float4 posW = mul(_Obj.model, float4(vsIn.pos, 1.0));
    psIn.posW = posW.xyz / posW.w;
    float4x4 normalMat = transpose(_Obj.modelInv);
    psIn.norW = mul((float3x3)normalMat, vsIn.nor);
    psIn.uv = vsIn.uv;
    return psIn;
}

float4 PSMain(PS_INPUT psIn) : SV_Target
{
    // float3 norW = normalize(psIn.norW);
    // return float4(norW * 0.5 + 0.5, 1.0);

    float3 posW = psIn.posW;
    Frame frame;
    frame.n = normalize(psIn.norW);
    coordinate_system(frame.n, frame.s, frame.t);

    float3 lightPos = float3(10.0, 10.0, 10.0);
    float3 lightDirW = normalize(lightPos - posW);
    float3 viewDirW = normalize(_Camera.posW - posW);

    float3 wi = to_local(frame, viewDirW);
    float3 wo = to_local(frame, lightDirW);

    DSPBRData dspbr = get_dspbr_data(_Mat);

    float3 specular = eval_brdf_microfacet_ggx(dspbr.alpha, wi, wo, dspbr.specular_f0, dspbr.specular_f90);

    float3 finalColor = specular;

    finalColor = pow(finalColor, (float3)(1.0 / 2.2));

    return float4(finalColor, 1.0);
}
