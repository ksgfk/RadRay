struct VertexInput {
    float3 position: POSITION;
    float3 normal: NORMAL;
    float2 uv0: TEXCOORD0;
};

struct V2P {
    float4 pos: SV_POSITION;
    float3 normal: NORMAL;
    float2 uv0: TEXCOORD0;
};

struct PreObject {
    float4x4 mvp;
    float4x4 model;
};

struct ObjectMaterial {
    float3 baseColor;
};

ConstantBuffer<PreObject> g_PreObject: register(b0);
ConstantBuffer<ObjectMaterial> g_Material: register(b1);

#ifdef BASE_COLOR_USE_TEXTURE
Texture2D<float4> g_BaseColor: register(t0);
SamplerState g_BaseColorSampler: register(s0);
#endif

V2P VSMain(VertexInput v) {
    V2P v2p;
    v2p.pos = mul(g_PreObject.mvp, float4(v.position, 1));
    v2p.normal = normalize(mul((float3x3)g_PreObject.model, v.normal));
    v2p.uv0 = v.uv0;
    return v2p;
}

float4 PSMain(V2P v2p) : SV_Target {
// #ifdef BASE_COLOR_USE_TEXTURE
//     float3 color = g_BaseColor.Sample(g_BaseColorSampler, v2p.uv0);
// #else
//     float3 color = g_Material.baseColor;
// #endif
    float3 color = normalize(v2p.normal) * 0.5 + 0.5;
    return float4(color, 1);
}
