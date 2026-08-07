#include <core/platform.hlsli>

#pragma radray_keyword_group DEPTH_MODE "regular" "reversed"

struct DepthVertexInput {
    float3 Position : POSITION;
};

[shader("vertex")]
float4 VSMain(DepthVertexInput input) : SV_Position {
    return float4(input.Position, 1.0f);
}
