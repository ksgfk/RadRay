#include <core/platform.hlsli>
VK_BINDING(0, 0) Texture2D<uint> SourceStencil : register(t0);
[shader("pixel")]
float4 PSMain(float4 position : SV_Position) : SV_Target0 { return float(SourceStencil.Load(int3(int2(position.xy), 0))).xxxx; }
