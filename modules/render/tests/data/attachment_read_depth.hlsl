#include <core/platform.hlsli>
VK_BINDING(0, 0) Texture2DMS<float> SourceDepth : register(t0);
VK_BINDING(1, 0) Texture2DMS<uint> SourceStencil : register(t1);
VK_BINDING(2, 0) Texture2D<float> TargetDepth : register(t2);
VK_BINDING(3, 0) Texture2D<uint> TargetStencil : register(t3);
VK_BINDING(4, 0) RWStructuredBuffer<float4> Output : register(u0);
[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    for (uint sample = 0; sample < 4; ++sample) Output[(id.y * 8 + id.x) * 4 + sample] = float4(SourceDepth.Load(id.xy, sample), SourceStencil.Load(id.xy, sample), TargetDepth.Load(int3(id.xy, 0)), TargetStencil.Load(int3(id.xy, 0)));
}
