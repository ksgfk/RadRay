#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <radray/render/dxc.h>
#include <radray/render/spvc.h>

using namespace radray;
using namespace radray::render;

static const char* IMGUI_HLSL = R"(
struct VS_INPUT {
    [[vk::location(0)]] float2 pos : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

struct PushConstant {
    float4x4 proj;
};

[[vk::push_constant]] ConstantBuffer<PushConstant> _PC : register(b0);
[[vk::binding(0, 0)]] Texture2D _Tex : register(t0);
[[vk::binding(1, 0)]] SamplerState _Sampler : register(s0);

PS_INPUT VSMain(VS_INPUT input) {
    PS_INPUT output;
    output.pos = mul(_PC.proj, float4(input.pos, 0.0f, 1.0f));
    output.color = input.color;
    output.uv = input.uv;
    return output;
}

float4 PSMain(PS_INPUT input) : SV_Target {
    return input.color * _Tex.Sample(_Sampler, input.uv);
}
)";

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <output_dir>\n", argv[0]);
        return 1;
    }
    std::filesystem::path outDir(argv[1]);
    std::filesystem::create_directories(outDir);

    auto dxc = CreateDxc();
    if (!dxc.HasValue()) {
        fprintf(stderr, "Failed to create DXC\n");
        return 1;
    }

    // Compile VS to SPIR-V
    auto vs = dxc->Compile(
        IMGUI_HLSL, "VSMain", ShaderStage::Vertex,
        HlslShaderModel::SM60, true, {}, {}, true);
    if (!vs.has_value()) {
        fprintf(stderr, "Failed to compile VS to SPIR-V\n");
        return 1;
    }

    // Compile PS to SPIR-V
    auto ps = dxc->Compile(
        IMGUI_HLSL, "PSMain", ShaderStage::Pixel,
        HlslShaderModel::SM60, true, {}, {}, true);
    if (!ps.has_value()) {
        fprintf(stderr, "Failed to compile PS to SPIR-V\n");
        return 1;
    }

    SpirvToMslOption mslOpt{};
    mslOpt.MslMajor = 3;
    mslOpt.MslMinor = 0;
    mslOpt.UseArgumentBuffers = true;

    // Convert VS SPIR-V to MSL
    auto vsMsl = ConvertSpirvToMsl(
        vs->Data, "VSMain", ShaderStage::Vertex, mslOpt);
    if (!vsMsl.has_value()) {
        fprintf(stderr, "Failed to convert VS SPIR-V to MSL\n");
        return 1;
    }

    // Convert PS SPIR-V to MSL
    auto psMsl = ConvertSpirvToMsl(
        ps->Data, "PSMain", ShaderStage::Pixel, mslOpt);
    if (!psMsl.has_value()) {
        fprintf(stderr, "Failed to convert PS SPIR-V to MSL\n");
        return 1;
    }

    // Write MSL source files
    auto vsPath = outDir / "imgui_vs.metal";
    auto psPath = outDir / "imgui_ps.metal";
    {
        std::ofstream f(vsPath);
        f << vsMsl->MslSource;
    }
    {
        std::ofstream f(psPath);
        f << psMsl->MslSource;
    }
    printf("VS MSL written to: %s\n", vsPath.c_str());
    printf("PS MSL written to: %s\n", psPath.c_str());
    printf("VS:\n%s\n", vsMsl->MslSource.c_str());
    printf("PS:\n%s\n", psMsl->MslSource.c_str());

    return 0;
}
