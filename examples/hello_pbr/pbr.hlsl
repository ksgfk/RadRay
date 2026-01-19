#ifdef VULKAN
#define VK_LOCATION(l)   [[vk::location(l)]]
#define VK_BINDING(b, s) [[vk::binding(b, s)]]
#define VK_PUSH_CONSTANT [[vk::push_constant]]
#else
#define VK_LOCATION(l)
#define VK_BINDING(b, s)
#define VK_PUSH_CONSTANT
#endif

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
    VK_LOCATION(0) float3 pos : POSITION;
    VK_LOCATION(1) float3 nor : NORMAL0;
    VK_LOCATION(2) float2 uv  : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    VK_LOCATION(0) float3 posW : POSITION;
    VK_LOCATION(1) float3 norW : NORMAL0;
    VK_LOCATION(2) float2 uv   : TEXCOORD0;
};

VK_PUSH_CONSTANT ConstantBuffer<PreObjectData> _Obj : register(b0);
VK_BINDING(0, 0) ConstantBuffer<PreCameraData> _Camera : register(b1);

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
    float3 n = normalize(psIn.norW);
    return float4(n*0.5 + 0.5, 1.0);
}
