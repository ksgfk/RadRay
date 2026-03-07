#include <common.hlsl>

struct ToneData
{
    float4 toneParams;
};

struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VK_PUSH_CONSTANT ConstantBuffer<ToneData> _Tone : register(b0);
VK_BINDING(0, 0) Texture2D _HDRTex : register(t0, space0);

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

float3 ACESFitted(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 PSMain(VS_OUTPUT input) : SV_Target
{
    float exposure = max(_Tone.toneParams.x, 1e-4);
    uint width = 0;
    uint height = 0;
    _HDRTex.GetDimensions(width, height);
    uint2 p = UVToPixel(input.uv, width, height);
    float3 hdr = _HDRTex.Load(int3(p, 0)).rgb * exposure;
    float3 mapped = ACESFitted(hdr);
    float3 srgb = pow(mapped, 1.0 / 2.2);
    return float4(srgb, 1.0);
}
