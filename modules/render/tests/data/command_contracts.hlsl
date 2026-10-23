struct VertexOutput {
    float4 Position : SV_Position;
    float4 Color : COLOR;
};

[shader("vertex")]
VertexOutput VSMain(float3 position : POSITION, float2 offset : TEXCOORD0, float4 color : COLOR) {
    VertexOutput result;
    result.Position = float4(position.xy + offset, position.z, 1);
    result.Color = color;
    return result;
}

struct PixelOutput {
    float4 First : SV_Target0;
    float4 Second : SV_Target1;
};

[shader("pixel")]
PixelOutput PSMain(VertexOutput input) {
    PixelOutput result;
    result.First = result.Second = input.Color;
    return result;
}
