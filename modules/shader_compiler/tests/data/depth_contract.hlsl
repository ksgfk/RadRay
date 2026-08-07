#include <core/platform.hlsli>

#pragma radray_keyword_group DEPTH_ONLY "off" "on"

[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position {
    return float4(position, 1.0);
}
