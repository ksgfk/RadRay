// Scale scan for the frame group memo: one material, one view, one vertex-data-free draw per object,
// each draw carrying its own MaterialConstants override so every draw reads a distinct resource list.
// A linear memo scan degrades to O(draws^2) here; the frame-level open-addressing index stays O(draws).
#include <core/platform.hlsli>
struct Values { float4 Data; };
VK_BINDING(0, 1) ConstantBuffer<Values> ViewData : register(b0, space1);
VK_BINDING(0, 2) ConstantBuffer<Values> Tint : register(b0, space2);
#define GROUPS "DescriptorTable(CBV(b0,space=1)),DescriptorTable(CBV(b0,space=2))"
struct Vertex { float4 Position : SV_Position; };
[RootSignature(GROUPS)]
[shader("vertex")] Vertex VSMain(uint index : SV_VertexID) {
    const float2 positions[3] = {float2(-1, -1), float2(-1, 3), float2(3, -1)};
    Vertex output;
    output.Position = float4(positions[index], ViewData.Data.x, 1);
    return output;
}
[RootSignature(GROUPS)]
[shader("pixel")] float4 PSMain(Vertex input) : SV_Target {
    return Tint.Data;
}
