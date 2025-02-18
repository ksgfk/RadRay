struct VertexInput {
    float3 position: POSITION;
    float3 normal: NORMAL;
};

struct V2P {
    float4 pos: SV_POSITION;
    float3 normal: NORMAL;
};

struct PreObject {
    float4x4 mvp;
};

ConstantBuffer<PreObject> g_PreObject: register(b0);

V2P VSMain(VertexInput v) {
    V2P v2p;
    v2p.pos = mul(float4(v.position, 1), g_PreObject.mvp);
    v2p.normal = normalize(mul(float4(v.normal, 0), g_PreObject.mvp)).xyz;
    return v2p;
}

float4 PSMain(V2P v2p) : SV_Target {
    float3 color = normalize(v2p.normal) * 0.5 + 0.5;
    return float4(color, 1);
}
