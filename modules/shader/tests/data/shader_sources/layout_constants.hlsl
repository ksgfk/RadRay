#include <core/platform.hlsli>

struct MatrixData {
    column_major float3x3 Matrices[2];
};
struct MaterialData {
    float Gain;
    float4 Tint;
};
VK_BINDING(0, 0) ConstantBuffer<MatrixData> MatrixBlock : register(b0);
VK_BINDING(1, 0) ConstantBuffer<MaterialData> MaterialBlock : register(b1);
VK_BINDING(2, 0) RWStructuredBuffer<float4> Output : register(u0);
VK_BINDING(3, 0) Texture2D<float4> Image : register(t0);
VK_BINDING(4, 0) SamplerState ImageSampler : register(s0);

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain() {
    for (uint matrix = 0; matrix < 2; ++matrix)
        for (uint row = 0; row < 3; ++row)
            Output[matrix * 3 + row] = float4(MatrixBlock.Matrices[matrix][row], 1);
    Output[6] = MaterialBlock.Tint * MaterialBlock.Gain * Image.SampleLevel(ImageSampler, float2(0.5, 0.5), 0);
}
