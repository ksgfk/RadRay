#include <core/platform.hlsli>
struct PushData { float4 Value; };
VK_PUSH_CONSTANT ConstantBuffer<PushData> Push : register(b0);
[shader("vertex")]
float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    return float4(vertex == 2 ? 3.0 : -1.0, vertex == 1 ? 3.0 : -1.0, Push.Value.x, 1.0);
}
