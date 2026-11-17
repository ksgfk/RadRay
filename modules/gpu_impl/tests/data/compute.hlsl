#include <core/platform.hlsli>

struct Constants {
    uint Value;
    uint Slot;
};

struct PushData {
    uint A;
    uint B;
};

VK_BINDING(0, 0) RWStructuredBuffer<uint> Output : register(u0);
VK_BINDING(1, 0) StructuredBuffer<uint> Input : register(t0);
VK_BINDING(2, 0) ConstantBuffer<Constants> Data : register(b0);
VK_BINDING(3, 0) Texture2D<float4> Image : register(t1);
VK_BINDING(4, 0) SamplerState PointSampler : register(s0);
VK_PUSH_CONSTANT ConstantBuffer<PushData> Push : register(b1);
VK_BINDING(0, 1) StructuredBuffer<uint> Material : register(t0, space1);
VK_BINDING(0, 2) StructuredBuffer<uint> Object : register(t0, space2);

uint PackUnorm(float4 color) {
    uint4 bytes = uint4(round(saturate(color) * 255.0));
    return bytes.r | (bytes.g << 8) | (bytes.b << 16) | (bytes.a << 24);
}

[shader("compute")]
[numthreads(4, 1, 1)]
void TransformInput(uint3 id : SV_DispatchThreadID) {
    Output[id.x] = Input[id.x] * 2 + 1;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void ReadConstants() {
    Output[Data.Slot] = Data.Value;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void ReadPushConstants() {
    Output[0] = Push.A;
    Output[1] = Push.B;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void SampleImage() {
    Output[0] = PackUnorm(Image.SampleLevel(PointSampler, float2(0.25, 0.25), 0));
    Output[1] = PackUnorm(Image.SampleLevel(PointSampler, float2(0.75, 0.75), 0));
}

[shader("compute")]
[numthreads(1, 1, 1)]
void CombineGroups() {
    Output[Object[1]] = Material[0] * 100 + Object[0];
}
