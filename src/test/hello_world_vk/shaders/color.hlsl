const static float3 g_Color[3] = {
    float3(1, 0, 0),
    float3(0, 1, 0),
    float3(0, 0, 1)};

struct V2P {
    float4 pos : SV_POSITION;
    [[vk::location(0)]] float3 color: COLOR0;
};

V2P VSMain([[vk::location(0)]] float3 v_vert: POSITION, uint vertId: SV_VertexID) {
    V2P v2p;
    v2p.pos = float4(v_vert, 1);
    v2p.color = g_Color[vertId % 3];
    return v2p;
}

float4 PSMain(V2P v2p) : SV_Target {
    return float4(v2p.color, 1);
}
