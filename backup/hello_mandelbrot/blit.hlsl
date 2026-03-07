#include <common.hlsl>

struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VS_OUTPUT VSMain(uint id : SV_VertexID) {
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

VK_BINDING(0, 0) Texture2D _Tex       : register(t0, space0);
VK_BINDING(1, 0) SamplerState _Sampler : register(s0, space0);

float4 PSMain(VS_OUTPUT input) : SV_TARGET {
    return _Tex.Sample(_Sampler, input.uv);
}
