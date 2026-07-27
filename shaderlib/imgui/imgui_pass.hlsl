// imgui_pass.hlsl —— 入口 shader (entry point)。
//
// 用途: Dear ImGui 的绘制 pass。
// 入口: VSMain (Vertex), PSMain (Pixel)。
//
// 顶点已是屏幕像素坐标, VS 只做 像素 -> NDC 的缩放平移 (由 ImGui 每帧算好塞进 push constant)。
// 顶点色为 sRGB UNORM8x4, 由硬件解码, 这里不做色彩空间变换。
//
// 这个文件同时是 tools/generate_imgui_shader.py 的输入 (预编译成 C++ 数组内嵌进 runtime),
// 因此【不得】引入任何 #include —— 那个脚本编译时不带 -I。

struct VertexInput {
    float2 Position : POSITION0;  // 屏幕像素坐标
    float2 UV : TEXCOORD0;
    float4 Color : COLOR0;
};

struct VertexOutput {
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
    float4 Color : COLOR0;
};

struct ImGuiPushConstants {
    float2 Scale;      // 像素 -> NDC 的缩放
    float2 Translate;  // 像素 -> NDC 的平移
};

[[vk::push_constant]] ConstantBuffer<ImGuiPushConstants> gPush : register(b0, space0);
[[vk::binding(0, 1)]] Texture2D<float4> gTexture : register(t0, space1);
[[vk::binding(1, 1)]] SamplerState gSampler : register(s1, space1);

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    output.Position = float4(input.Position * gPush.Scale + gPush.Translate, 0.0f, 1.0f);
    output.UV = input.UV;
    output.Color = input.Color;
    return output;
}

float4 PSMain(VertexOutput input) : SV_Target0 {
    return input.Color * gTexture.Sample(gSampler, input.UV);
}
