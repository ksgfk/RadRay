#include <core/platform.hlsli>

// 与 C++ DrawConstants 一致：Eigen 的列主序矩阵可直接按 16 个 float 上传。
// 本例用统一的 VS 处理两个 Pass，确保预深度与颜色 Pass 的位置计算一致。
struct PushData {
    column_major float4x4 LocalToClip;
    float4 Color;
};

VK_PUSH_CONSTANT ConstantBuffer<PushData> Push : register(b0);

[shader("vertex")]
float4 VSMain(VK_LOCATION(0) float3 position : POSITION) : SV_Position {
    return mul(Push.LocalToClip, float4(position, 1.0));
}

[shader("pixel")]
float4 PSMain() : SV_Target0 {
    return Push.Color;
}
