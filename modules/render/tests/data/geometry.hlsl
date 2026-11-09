#include <core/platform.hlsli>

[shader("vertex")]
float4 VSMain(VK_LOCATION(0) float3 position : POSITION) : SV_Position {
    return float4(position, 1.0f);
}
