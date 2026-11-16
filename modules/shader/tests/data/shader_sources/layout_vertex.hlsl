#include <core/platform.hlsli>

struct VertexInput {
    VK_LOCATION(5) float2 Position : POSITION;
    VK_LOCATION(2) float4 Color : COLOR;
};
struct PixelInput {
    float4 Position : SV_Position;
    float4 Color : COLOR;
};
[shader("vertex")]
PixelInput VSMain(VertexInput input) {
    PixelInput result;
    result.Position = float4(input.Position, 0.5, 1);
    result.Color = input.Color;
    return result;
}
[shader("pixel")]
float4 PSMain(PixelInput input) : SV_Target0 { return input.Color; }
