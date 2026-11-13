#include <core/platform.hlsli>

struct TextureSelection {
    uint Mip;
    uint Layer;
    uint Width;
    uint OutputOffset;
};

VK_BINDING(0, 0) Texture2DArray<uint> Image : register(t0);
VK_BINDING(1, 0) RWStructuredBuffer<uint> Output : register(u0);
VK_PUSH_CONSTANT ConstantBuffer<TextureSelection> Selection : register(b0);

[shader("compute")]
[numthreads(1, 1, 1)]
void LoadUploadedTexture(uint3 id : SV_DispatchThreadID) {
    Output[Selection.OutputOffset + id.y * Selection.Width + id.x] = Image.Load(int4(id.xy, Selection.Layer, Selection.Mip));
}
