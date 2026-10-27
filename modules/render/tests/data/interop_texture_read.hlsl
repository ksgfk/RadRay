#include <core/platform.hlsli>
struct PushData { uint Seed, Width, Height, Depth, Mip, Layer, Samples, Reserved; };
VK_PUSH_CONSTANT ConstantBuffer<PushData> Push : register(b0);
#if DIMENSION == 1
VK_BINDING(0, 0) Texture1DArray<VALUE_TYPE> Source : register(t0);
#elif DIMENSION == 3
VK_BINDING(0, 0) Texture3D<VALUE_TYPE> Source : register(t0);
#elif DIMENSION == 4
VK_BINDING(0, 0) TextureCube<VALUE_TYPE> Source : register(t0);
VK_BINDING(2, 0) SamplerState SourceSampler : register(s0);
#elif DIMENSION == 5
VK_BINDING(0, 0) TextureCubeArray<VALUE_TYPE> Source : register(t0);
VK_BINDING(2, 0) SamplerState SourceSampler : register(s0);
#elif DIMENSION == 6
VK_BINDING(0, 0) Texture2DMSArray<VALUE_TYPE> Source : register(t0);
#else
VK_BINDING(0, 0) Texture2DArray<VALUE_TYPE> Source : register(t0);
#endif
VK_BINDING(1, 0) RWStructuredBuffer<float4> Output : register(u0);
float3 FaceDirection(uint face, float2 p) {
    if (face == 0) return float3(1, -p.y, -p.x);
    if (face == 1) return float3(-1, -p.y, p.x);
    if (face == 2) return float3(p.x, 1, p.y);
    if (face == 3) return float3(p.x, -1, -p.y);
    if (face == 4) return float3(p.x, -p.y, 1);
    return float3(-p.x, -p.y, -1);
}
[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Push.Width || id.y >= Push.Height || id.z >= Push.Depth) return;
    for (uint sample = 0; sample < Push.Samples; ++sample) {
#if DIMENSION == 1
        float4 value = Source.Load(int3(id.x, 0, 0));
#elif DIMENSION == 3
        float4 value = Source.Load(int4(id, 0));
#elif DIMENSION == 4
        float4 value = Source.SampleLevel(SourceSampler, FaceDirection(Push.Layer % 6, (float2(id.xy) + 0.5) / float2(Push.Width, Push.Height) * 2 - 1), 0);
#elif DIMENSION == 5
        float4 value = Source.SampleLevel(SourceSampler, float4(FaceDirection(Push.Layer % 6, (float2(id.xy) + 0.5) / float2(Push.Width, Push.Height) * 2 - 1), Push.Layer / 6), 0);
#elif DIMENSION == 6
        float4 value = Source.Load(int3(id.xy, 0), sample);
#else
        float4 value = Source.Load(int4(id.xy, 0, 0));
#endif
        Output[((id.z * Push.Height + id.y) * Push.Width + id.x) * Push.Samples + sample] = value;
    }
}
