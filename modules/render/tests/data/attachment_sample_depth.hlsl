#include <core/platform.hlsli>
VK_BINDING(0, 0) Texture2D<float> SourceDepth : register(t0);
[shader("vertex")]
float4 VSMain(uint vertex : SV_VertexID) : SV_Position { return float4(vertex == 2 ? 3.0 : -1.0, vertex == 1 ? 3.0 : -1.0, 0.5, 1); }
[shader("pixel")]
float4 PSMain(float4 position : SV_Position) : SV_Target0 { return SourceDepth.Load(int3(int2(position.xy), 0)).xxxx; }
