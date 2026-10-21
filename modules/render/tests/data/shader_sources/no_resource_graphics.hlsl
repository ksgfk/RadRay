[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return float4(1.0, 0.0, 1.0, 1.0);
}
