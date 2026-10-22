#include <core/platform.hlsli>
struct Constants { uint Value; };
#if defined(__spirv__)
struct Push { uint A; uint B; };
VK_PUSH_CONSTANT ConstantBuffer<Push> Parameters : register(b9);
#else
ConstantBuffer<Constants> First : register(b9);
ConstantBuffer<Constants> Second : register(b10);
#endif
VK_BINDING(6, 0) RWStructuredBuffer<uint> Output : register(u2);
[shader("compute")]
[numthreads(1, 1, 1)]
void CSMain() {
#if defined(__spirv__)
    Output[0] = Parameters.A + Parameters.B;
#else
    Output[0] = First.Value + Second.Value;
#endif
}
