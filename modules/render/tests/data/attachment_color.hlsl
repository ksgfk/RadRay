#include <core/platform.hlsli>
struct PushData { float4 Value; };
VK_PUSH_CONSTANT ConstantBuffer<PushData> Push : register(b0);
[shader("vertex")]
float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    return float4(vertex == 2 ? 3.0 : -1.0, vertex == 1 ? 3.0 : -1.0, 0.5, 1.0);
}
[shader("pixel")]
float4 PSMain() : SV_Target0 { return Push.Value; }
