struct ImGuiVertexInput {
    float2 Position : POSITION0;
    float2 UV : TEXCOORD0;
    float4 Color : COLOR0;
};

struct ImGuiVertexOutput {
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
    float4 Color : COLOR0;
};

struct ImGuiPushConstants {
    float2 Scale;
    float2 Translate;
};

[[vk::push_constant]] ConstantBuffer<ImGuiPushConstants> gPush : register(b0, space0);
[[vk::binding(0, 1)]] Texture2D<float4> gTexture : register(t0, space1);
[[vk::binding(1, 1)]] SamplerState gSampler : register(s1, space1);

ImGuiVertexOutput VSMain(ImGuiVertexInput input) {
    ImGuiVertexOutput output;
    output.Position = float4(input.Position * gPush.Scale + gPush.Translate, 0.0, 1.0);
    output.UV = input.UV;
    output.Color = input.Color;
    return output;
}

float4 PSMain(ImGuiVertexOutput input) : SV_Target0 {
    return input.Color * gTexture.Sample(gSampler, input.UV);
}
