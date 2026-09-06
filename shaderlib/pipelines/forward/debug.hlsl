#include <pipelines/forward/effects.hlsli>
VK_BINDING(12, 0) StructuredBuffer<uint2> DebugHeaders : register(t6);
VK_BINDING(13, 0) Texture2DArray<float> DebugShadows : register(t7);
[shader("compute")] [numthreads(8, 8, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= (uint2)Effects.Extent.xy)) return;
    float3 color;
    if (Effects.DebugMode == 5) {
        uint2 tile = id.xy / 16;
        uint2 header = DebugHeaders[tile.y * (((uint)Effects.Extent.x + 15) / 16) + tile.x];
        float fraction = (float)header.x / max(1, Effects.TileCapacity);
        color = header.y != 0 ? float3(1, 0, 1) : lerp(float3(.015, .03, .1), float3(1, .4, .02), fraction);
    } else {
        float2 uv = uv_of(id.xy) * 2;
        uint layer = (uint)uv.y * 2 + (uint)uv.x;
        float depth = DebugShadows.SampleLevel(ClampSampler, float3(frac(uv), layer), 0);
        color = (1 - depth).xxx;
    }
    OutputColor[id.xy] = float4(color, 1);
}
