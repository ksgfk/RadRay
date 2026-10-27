#include <core/platform.hlsli>
struct PushData { uint Seed, Width, Height, Format; };
VK_PUSH_CONSTANT ConstantBuffer<PushData> Push : register(b0);
VK_BINDING(0, 0) Texture2D<float4> Source : register(t0);
VK_BINDING(1, 0) RWStructuredBuffer<float4> Output : register(u0);
VK_BINDING(2, 0) SamplerState SourceSampler : register(s0);
[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x < Push.Width && id.y < Push.Height)
        Output[id.y * Push.Width + id.x] = Source.SampleLevel(SourceSampler, (float2(id.xy) + 0.5) / float2(Push.Width, Push.Height), 0);
}
