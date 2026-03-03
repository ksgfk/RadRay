#include <common.hlsl>

struct LightingData
{
    float4x4 invViewProj;
    float4 cameraPos;
    float4 lightDirIntensity;
    float4 lightColorAmbient;
    float4 screenAndDebug;
};

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VK_PUSH_CONSTANT ConstantBuffer<LightingData> _Lighting : register(b0);

VK_BINDING(0, 0) Texture2D _GAlbedo   : register(t0, space0);
VK_BINDING(1, 0) Texture2D _GNormal   : register(t1, space0);
VK_BINDING(2, 0) Texture2D _GMaterial : register(t2, space0);
VK_BINDING(3, 0) Texture2D _GDepth    : register(t3, space0);

uint2 UVToPixel(float2 uv, uint width, uint height)
{
    float2 clamped = saturate(uv);
    uint2 p = uint2(clamped * float2(width, height));
    p.x = min(p.x, max(width, 1u) - 1u);
    p.y = min(p.y, max(height, 1u) - 1u);
    return p;
}

VS_OUTPUT VSMain(uint id : SV_VertexID)
{
    float2 uv = float2((id & 1) * 2.0, (id >> 1) * 2.0);
#ifdef VULKAN
    float4 pos = float4(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, 0.0, 1.0);
#else
    float4 pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
#endif
    VS_OUTPUT output;
    output.pos = pos;
    output.uv = uv;
    return output;
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
    float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    float4 world = mul(_Lighting.invViewProj, clip);
    return world.xyz / max(world.w, 1e-6);
}

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    uint width = 0;
    uint height = 0;
    _GAlbedo.GetDimensions(width, height);
    uint2 p = UVToPixel(input.uv, width, height);

    float3 albedo = _GAlbedo.Load(int3(p, 0)).rgb;
    float3 normal = normalize(_GNormal.Load(int3(p, 0)).xyz * 2.0 - 1.0);
    float2 material = _GMaterial.Load(int3(p, 0)).rg;
    float depth = _GDepth.Load(int3(p, 0)).r;

    uint debugView = (uint)round(_Lighting.screenAndDebug.z);
    if (debugView == 1) {
        return float4(albedo, 1.0);
    }
    if (debugView == 2) {
        return float4(normal * 0.5 + 0.5, 1.0);
    }
    if (debugView == 3) {
        return float4(material, 0.0, 1.0);
    }
    if (debugView == 4) {
        return float4(depth.xxx, 1.0);
    }

    float3 worldPos = ReconstructWorldPos(input.uv, depth);
    float3 N = normal;
    float3 L = normalize(-_Lighting.lightDirIntensity.xyz);
    float3 V = normalize(_Lighting.cameraPos.xyz - worldPos);
    float3 H = normalize(L + V);

    float roughness = max(material.x, 0.04);
    float metallic = saturate(material.y);
    float ndotl = saturate(dot(N, L));
    float ndoth = saturate(dot(N, H));
    float shininess = lerp(128.0, 8.0, roughness);
    float spec = pow(ndoth, shininess);

    float3 lightColor = _Lighting.lightColorAmbient.rgb * _Lighting.lightDirIntensity.w;
    float ambient = _Lighting.lightColorAmbient.w;

    float3 diffuse = albedo * ndotl;
    float3 specular = lerp(spec.xxx, albedo * spec, metallic);
    float3 color = (diffuse + specular) * lightColor + albedo * ambient;
    return float4(max(color, 0.0), 1.0);
}
