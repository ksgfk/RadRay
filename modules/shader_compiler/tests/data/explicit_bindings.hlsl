#include <core/platform.hlsli>

struct Constants { uint Value; };
struct Push { uint A; uint B; };
VK_BINDING(6, 3) RWStructuredBuffer<uint> Output : register(u2);
VK_BINDING(8, 3) StructuredBuffer<uint> Input : register(t4);
VK_BINDING(1, 3) ByteAddressBuffer Raw : register(t6);
VK_BINDING(2, 3) Buffer<uint> Typed : register(t7);
VK_BINDING(4, 3) ConstantBuffer<Constants> Data : register(b1);
VK_BINDING(10, 3) StructuredBuffer<uint> Array[2] : register(t8);
VK_BINDING(12, 3) RWByteAddressBuffer RawOutput : register(u4);
VK_BINDING(13, 3) RWBuffer<uint> TypedOutput : register(u5);
VK_BINDING(14, 3) Texture2D<float4> Texture : register(t10);
VK_BINDING(15, 3) RWTexture2D<float4> Image : register(u6);
VK_BINDING(16, 3) SamplerState Linear : register(s0);
VK_BINDING(17, 3) SamplerState Fixed : register(s1);
VK_PUSH_CONSTANT ConstantBuffer<Push> Parameters : register(b9);

[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain() {
    uint value = Input[0] + Raw.Load(0) + Typed[0] + Data.Value + Array[0][0] + Array[1][0];
    value += uint(Texture.SampleLevel(Linear, float2(0.5, 0.5), 0).r);
    value += uint(Texture.SampleLevel(Fixed, float2(0.5, 0.5), 0).r);
    value += Parameters.A + Parameters.B;
    Output[0] = value;
    RawOutput.Store(0, value + 1);
    TypedOutput[0] = value + 2;
    Image[uint2(0, 0)] = float4(value, 0, 0, 1);
}
