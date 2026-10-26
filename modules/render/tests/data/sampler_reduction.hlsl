#include <core/platform.hlsli>
VK_BINDING(0, 0) Texture2D<float> Source : register(t0);
VK_BINDING(1, 0) SamplerState Filter : register(s0);
VK_BINDING(2, 0) RWStructuredBuffer<float> Output : register(u0);
[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain() { Output[0] = Source.SampleLevel(Filter, float2(0.5, 0.5), 0); }
