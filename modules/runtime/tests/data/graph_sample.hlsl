#include <core/platform.hlsli>
VK_BINDING(0, 0) Texture2D<float4> inputImage : register(t0);
[shader("vertex")]
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 position = float2((id << 1) & 2, id & 2);
    return float4(position * 2 - 1, 0, 1);
}
[shader("pixel")]
float4 PSMain(float4 position : SV_Position) : SV_Target0 {
    return inputImage.Load(int3(int2(position.xy), 0));
}
