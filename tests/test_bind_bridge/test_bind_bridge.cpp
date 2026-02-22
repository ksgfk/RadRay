#include <gtest/gtest.h>
#include <iostream>

#include <radray/render/dxc.h>
#include <radray/render/spvc.h>
#include <radray/render/msl.h>
#include <radray/render/bind_bridge.h>

using namespace radray;
using namespace radray::render;

// ============================================================
// Shader sources
// ============================================================

// Compute shader: cbuffer + RWStructuredBuffer
static const char* HLSL_COMPUTE = R"(
cbuffer Params : register(b0)
{
    float4 color;
    float intensity;
};

RWStructuredBuffer<float4> _Output : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    _Output[dtid.x] = color * intensity;
}
)";

// VS/PS shader: ConstantBuffer + Texture + Sampler
static const char* HLSL_RENDER = R"(
struct VS_INPUT
{
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

struct PreObject
{
    float4x4 mvp;
};

ConstantBuffer<PreObject> _Obj : register(b0);
Texture2D _Tex : register(t0);
SamplerState _Sampler : register(s0);

PS_INPUT VSMain(VS_INPUT vsIn)
{
    PS_INPUT o;
    o.pos = mul(float4(vsIn.pos, 1.0f), _Obj.mvp);
    o.uv = vsIn.uv;
    return o;
}

float4 PSMain(PS_INPUT psIn) : SV_Target
{
    return _Tex.Sample(_Sampler, psIn.uv);
}
)";

// VS/PS with push constant (for SPIR-V path)
static const char* HLSL_RENDER_PUSHCONST = R"(
struct VS_INPUT
{
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float2 uv  : TEXCOORD0;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

struct PreObject
{
    float4x4 mvp;
};

[[vk::push_constant]] ConstantBuffer<PreObject> _Obj : register(b0);
[[vk::binding(0)]] Texture2D _Tex : register(t0);
[[vk::binding(1)]] SamplerState _Sampler : register(s0);

PS_INPUT VSMain(VS_INPUT vsIn)
{
    PS_INPUT o;
    o.pos = mul(float4(vsIn.pos, 1.0f), _Obj.mvp);
    o.uv = vsIn.uv;
    return o;
}

float4 PSMain(PS_INPUT psIn) : SV_Target
{
    return _Tex.Sample(_Sampler, psIn.uv);
}
)";

// ============================================================
// Helper: compile HLSL to DXIL and get HlslShaderDesc
// ============================================================
static std::optional<HlslShaderDesc> CompileHlslDesc(
    Dxc& dxc,
    const char* hlsl,
    const char* entry,
    ShaderStage stage) {
    auto out = dxc.Compile(hlsl, entry, stage, HlslShaderModel::SM60, true);
    if (!out.has_value()) return std::nullopt;
    return dxc.GetShaderDescFromOutput(stage, out->Refl, out->ReflExt);
}

// ============================================================
// Helper: compile HLSL to SPIR-V
// ============================================================
static std::optional<DxcOutput> CompileSpirv(
    Dxc& dxc,
    const char* hlsl,
    const char* entry,
    ShaderStage stage) {
    return dxc.Compile(hlsl, entry, stage, HlslShaderModel::SM60, true, {}, {}, true);
}

// ============================================================
// Helper: compile HLSL to SPIR-V and get SpirvShaderDesc
// ============================================================
static std::optional<SpirvShaderDesc> CompileSpirvDesc(
    Dxc& dxc,
    const char* hlsl,
    std::span<std::pair<const char*, ShaderStage>> entries) {
    std::vector<DxcOutput> outputs;
    std::vector<SpirvBytecodeView> views;
    std::vector<const DxcReflectionRadrayExt*> exts;
    for (auto& [entry, stage] : entries) {
        auto out = CompileSpirv(dxc, hlsl, entry, stage);
        if (!out.has_value()) return std::nullopt;
        outputs.push_back(std::move(*out));
    }
    for (size_t i = 0; i < outputs.size(); i++) {
        views.push_back({outputs[i].Data, entries[i].first, entries[i].second});
        exts.push_back(&outputs[i].ReflExt);
    }
    return ReflectSpirv(views, exts);
}

// ============================================================
// Helper: compile HLSL to SPIR-V and get MslShaderReflection
// ============================================================
static std::optional<MslShaderReflection> CompileMslRefl(
    Dxc& dxc,
    const char* hlsl,
    std::span<std::pair<const char*, ShaderStage>> entries,
    bool useArgBuffers = false) {
    std::vector<DxcOutput> outputs;
    std::vector<MslReflectParams> params;
    for (auto& [entry, stage] : entries) {
        auto out = CompileSpirv(dxc, hlsl, entry, stage);
        if (!out.has_value()) return std::nullopt;
        outputs.push_back(std::move(*out));
    }
    for (size_t i = 0; i < outputs.size(); i++) {
        params.push_back({outputs[i].Data, entries[i].first, entries[i].second, useArgBuffers});
    }
    return ReflectMsl(params);
}

// ============================================================
// BindBridgeLayout — HLSL construction
// ============================================================

TEST(BindBridgeLayout, HlslComputeBasic) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    auto csDesc = CompileHlslDesc(*dxc, HLSL_COMPUTE, "CSMain", ShaderStage::Compute);
    ASSERT_TRUE(csDesc.has_value());

    BindBridgeLayout layout(*csDesc);
    auto bindings = layout.GetBindings();
    EXPECT_FALSE(bindings.empty());

    // Should find Params (cbuffer) and _Output (RWStructuredBuffer)
    auto paramsId = layout.GetBindingId("Params");
    auto outputId = layout.GetBindingId("_Output");
    EXPECT_TRUE(paramsId.has_value()) << "Params binding not found";
    EXPECT_TRUE(outputId.has_value()) << "_Output binding not found";
    EXPECT_NE(paramsId.value(), outputId.value());
}

TEST(BindBridgeLayout, HlslComputeDescriptor) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    auto csDesc = CompileHlslDesc(*dxc, HLSL_COMPUTE, "CSMain", ShaderStage::Compute);
    ASSERT_TRUE(csDesc.has_value());

    BindBridgeLayout layout(*csDesc);
    auto container = layout.GetDescriptor();
    const auto& desc = container.Get();

    // Should have some root descriptors or descriptor sets
    bool hasContent = !desc.RootDescriptors.empty() || !desc.DescriptorSets.empty() || desc.Constant.has_value();
    EXPECT_TRUE(hasContent) << "descriptor should have content";
}

TEST(BindBridgeLayout, HlslRenderMerged) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    auto vsDesc = CompileHlslDesc(*dxc, HLSL_RENDER, "VSMain", ShaderStage::Vertex);
    ASSERT_TRUE(vsDesc.has_value());
    auto psDesc = CompileHlslDesc(*dxc, HLSL_RENDER, "PSMain", ShaderStage::Pixel);
    ASSERT_TRUE(psDesc.has_value());

    const HlslShaderDesc* descs[] = {&*vsDesc, &*psDesc};
    auto merged = MergeHlslShaderDesc(descs);
    ASSERT_TRUE(merged.has_value());

    BindBridgeLayout layout(*merged);
    auto bindings = layout.GetBindings();
    EXPECT_FALSE(bindings.empty());

    // Should find _Obj, _Tex, _Sampler
    EXPECT_TRUE(layout.GetBindingId("_Obj").has_value());
    EXPECT_TRUE(layout.GetBindingId("_Tex").has_value());
    EXPECT_TRUE(layout.GetBindingId("_Sampler").has_value());
}

TEST(BindBridgeLayout, HlslRenderDescriptorSets) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    auto vsDesc = CompileHlslDesc(*dxc, HLSL_RENDER, "VSMain", ShaderStage::Vertex);
    ASSERT_TRUE(vsDesc.has_value());
    auto psDesc = CompileHlslDesc(*dxc, HLSL_RENDER, "PSMain", ShaderStage::Pixel);
    ASSERT_TRUE(psDesc.has_value());

    const HlslShaderDesc* descs[] = {&*vsDesc, &*psDesc};
    auto merged = MergeHlslShaderDesc(descs);
    ASSERT_TRUE(merged.has_value());

    BindBridgeLayout layout(*merged);
    auto container = layout.GetDescriptor();
    const auto& desc = container.Get();

    // Texture and Sampler should end up in descriptor sets
    bool hasDescSets = !desc.DescriptorSets.empty();
    EXPECT_TRUE(hasDescSets) << "should have descriptor sets for texture/sampler";
}

TEST(BindBridgeLayout, HlslStaticSampler) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    auto vsDesc = CompileHlslDesc(*dxc, HLSL_RENDER, "VSMain", ShaderStage::Vertex);
    ASSERT_TRUE(vsDesc.has_value());
    auto psDesc = CompileHlslDesc(*dxc, HLSL_RENDER, "PSMain", ShaderStage::Pixel);
    ASSERT_TRUE(psDesc.has_value());

    const HlslShaderDesc* descs[] = {&*vsDesc, &*psDesc};
    auto merged = MergeHlslShaderDesc(descs);
    ASSERT_TRUE(merged.has_value());

    SamplerDescriptor sampDesc{};
    BindBridgeStaticSampler ss{"_Sampler", {sampDesc}};
    BindBridgeLayout layout(*merged, std::span{&ss, 1});

    auto container = layout.GetDescriptor();
    const auto& desc = container.Get();
    EXPECT_FALSE(desc.StaticSamplers.empty()) << "should have static samplers";
}

TEST(BindBridgeLayout, HlslEmptyShader) {
    HlslShaderDesc emptyDesc{};
    BindBridgeLayout layout(emptyDesc);
    EXPECT_TRUE(layout.GetBindings().empty());
    EXPECT_FALSE(layout.GetBindingId("anything").has_value());
}

// ============================================================
// BindBridgeLayout — SPIR-V construction
// ============================================================

TEST(BindBridgeLayout, SpvComputeBasic) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    std::pair<const char*, ShaderStage> entries[] = {{"CSMain", ShaderStage::Compute}};
    auto spvDesc = CompileSpirvDesc(*dxc, HLSL_COMPUTE, entries);
    ASSERT_TRUE(spvDesc.has_value());

    BindBridgeLayout layout(*spvDesc);
    auto bindings = layout.GetBindings();
    EXPECT_FALSE(bindings.empty());
}

TEST(BindBridgeLayout, SpvComputeDescriptor) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    std::pair<const char*, ShaderStage> entries[] = {{"CSMain", ShaderStage::Compute}};
    auto spvDesc = CompileSpirvDesc(*dxc, HLSL_COMPUTE, entries);
    ASSERT_TRUE(spvDesc.has_value());

    BindBridgeLayout layout(*spvDesc);
    auto container = layout.GetDescriptor();
    const auto& desc = container.Get();

    bool hasContent = !desc.RootDescriptors.empty() || !desc.DescriptorSets.empty() || desc.Constant.has_value();
    EXPECT_TRUE(hasContent);
}

TEST(BindBridgeLayout, SpvRenderPushConst) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    std::pair<const char*, ShaderStage> entries[] = {
        {"VSMain", ShaderStage::Vertex},
        {"PSMain", ShaderStage::Pixel}};
    auto spvDesc = CompileSpirvDesc(*dxc, HLSL_RENDER_PUSHCONST, entries);
    ASSERT_TRUE(spvDesc.has_value());

    BindBridgeLayout layout(*spvDesc);
    auto bindings = layout.GetBindings();
    EXPECT_FALSE(bindings.empty());

    // Should have push constant for _Obj
    auto container = layout.GetDescriptor();
    const auto& desc = container.Get();
    EXPECT_TRUE(desc.Constant.has_value()) << "should have push constant";
    if (desc.Constant.has_value()) {
        EXPECT_GT(desc.Constant->Size, 0u);
    }
}

TEST(BindBridgeLayout, SpvRenderDescriptorSets) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    std::pair<const char*, ShaderStage> entries[] = {
        {"VSMain", ShaderStage::Vertex},
        {"PSMain", ShaderStage::Pixel}};
    auto spvDesc = CompileSpirvDesc(*dxc, HLSL_RENDER_PUSHCONST, entries);
    ASSERT_TRUE(spvDesc.has_value());

    BindBridgeLayout layout(*spvDesc);
    auto container = layout.GetDescriptor();
    const auto& desc = container.Get();

    EXPECT_FALSE(desc.DescriptorSets.empty()) << "should have descriptor sets";
}

TEST(BindBridgeLayout, SpvStaticSampler) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    std::pair<const char*, ShaderStage> entries[] = {
        {"VSMain", ShaderStage::Vertex},
        {"PSMain", ShaderStage::Pixel}};
    auto spvDesc = CompileSpirvDesc(*dxc, HLSL_RENDER_PUSHCONST, entries);
    ASSERT_TRUE(spvDesc.has_value());

    SamplerDescriptor sampDesc{};
    BindBridgeStaticSampler ss{"_Sampler", {sampDesc}};
    BindBridgeLayout layout(*spvDesc, std::span{&ss, 1});

    auto container = layout.GetDescriptor();
    const auto& desc = container.Get();
    EXPECT_FALSE(desc.StaticSamplers.empty()) << "should have static samplers";
}

TEST(BindBridgeLayout, SpvEmpty) {
    SpirvShaderDesc emptyDesc{};
    BindBridgeLayout layout(emptyDesc);
    EXPECT_TRUE(layout.GetBindings().empty());
}

// ============================================================
// BindBridgeLayout — MSL construction
// ============================================================

TEST(BindBridgeLayout, MslComputeBasic) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    std::pair<const char*, ShaderStage> entries[] = {{"CSMain", ShaderStage::Compute}};
    auto mslRefl = CompileMslRefl(*dxc, HLSL_COMPUTE, entries);
    ASSERT_TRUE(mslRefl.has_value());

    BindBridgeLayout layout(*mslRefl);
    auto bindings = layout.GetBindings();
    EXPECT_FALSE(bindings.empty());
}

TEST(BindBridgeLayout, MslComputeDescriptor) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    std::pair<const char*, ShaderStage> entries[] = {{"CSMain", ShaderStage::Compute}};
    auto mslRefl = CompileMslRefl(*dxc, HLSL_COMPUTE, entries);
    ASSERT_TRUE(mslRefl.has_value());

    BindBridgeLayout layout(*mslRefl);
    auto container = layout.GetDescriptor();
    const auto& desc = container.Get();

    bool hasContent = !desc.DescriptorSets.empty();
    EXPECT_TRUE(hasContent) << "MSL layout should have descriptor sets";
}

TEST(BindBridgeLayout, MslRenderBasic) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    std::pair<const char*, ShaderStage> entries[] = {
        {"VSMain", ShaderStage::Vertex},
        {"PSMain", ShaderStage::Pixel}};
    auto mslRefl = CompileMslRefl(*dxc, HLSL_RENDER_PUSHCONST, entries);
    ASSERT_TRUE(mslRefl.has_value());

    BindBridgeLayout layout(*mslRefl);
    auto bindings = layout.GetBindings();
    EXPECT_FALSE(bindings.empty());

    // MSL should have descriptor sets (resources + samplers separated)
    auto container = layout.GetDescriptor();
    const auto& desc = container.Get();
    EXPECT_FALSE(desc.DescriptorSets.empty());
}

TEST(BindBridgeLayout, MslRenderArgBuffers) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    std::pair<const char*, ShaderStage> entries[] = {
        {"VSMain", ShaderStage::Vertex},
        {"PSMain", ShaderStage::Pixel}};
    auto mslRefl = CompileMslRefl(*dxc, HLSL_RENDER_PUSHCONST, entries, true);
    ASSERT_TRUE(mslRefl.has_value());

    for (const auto& arg : mslRefl->Arguments) {
        RADRAY_INFO_LOG("ArgBuffer Refl: name={} stage={} type={} index={} isPushConst={} descSet={}",
                        arg.Name, format_as(arg.Stage), format_as(arg.Type), arg.Index, arg.IsPushConstant, arg.DescriptorSet);
    }

    BindBridgeLayout layout(*mslRefl);
    auto container = layout.GetDescriptor();
    const auto& desc = container.Get();

    // Should have push constant
    EXPECT_TRUE(desc.Constant.has_value()) << "Should have push constant for _Obj";

    // Should have descriptor sets
    EXPECT_FALSE(desc.DescriptorSets.empty());
    for (size_t i = 0; i < desc.DescriptorSets.size(); i++) {
        const auto& set = desc.DescriptorSets[i];
        for (const auto& elem : set.Elements) {
            RADRAY_INFO_LOG("DescSet[{}] elem: slot={} type={}", i, elem.Slot, static_cast<int>(elem.Type));
        }
    }
}

TEST(BindBridgeLayout, MslRenderSamplerSeparation) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    std::pair<const char*, ShaderStage> entries[] = {
        {"VSMain", ShaderStage::Vertex},
        {"PSMain", ShaderStage::Pixel}};
    auto mslRefl = CompileMslRefl(*dxc, HLSL_RENDER_PUSHCONST, entries);
    ASSERT_TRUE(mslRefl.has_value());

    BindBridgeLayout layout(*mslRefl);
    auto bindings = layout.GetBindings();

    // MSL separates resources and samplers into different sets
    bool hasSamplerBinding = false;
    bool hasNonSamplerBinding = false;
    for (const auto& b : bindings) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, BindBridgeLayout::DescriptorSetEntry>) {
                if (e.Type == ResourceBindType::Sampler) {
                    hasSamplerBinding = true;
                } else {
                    hasNonSamplerBinding = true;
                }
            }
        }, b);
    }
    EXPECT_TRUE(hasSamplerBinding) << "should have sampler binding";
    EXPECT_TRUE(hasNonSamplerBinding) << "should have non-sampler binding";
}

TEST(BindBridgeLayout, MslStaticSampler) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    std::pair<const char*, ShaderStage> entries[] = {
        {"VSMain", ShaderStage::Vertex},
        {"PSMain", ShaderStage::Pixel}};
    auto mslRefl = CompileMslRefl(*dxc, HLSL_RENDER_PUSHCONST, entries);
    ASSERT_TRUE(mslRefl.has_value());

    SamplerDescriptor sampDesc{};
    BindBridgeStaticSampler ss{"_Sampler", {sampDesc}};
    BindBridgeLayout layout(*mslRefl, std::span{&ss, 1});

    auto container = layout.GetDescriptor();
    const auto& desc = container.Get();
    EXPECT_FALSE(desc.StaticSamplers.empty()) << "should have static samplers";
}

TEST(BindBridgeLayout, MslEmpty) {
    MslShaderReflection emptyRefl{};
    BindBridgeLayout layout(emptyRefl);
    EXPECT_TRUE(layout.GetBindings().empty());
}

// ============================================================
// Cross-path consistency: all three paths should produce
// compatible layouts for the same shader
// ============================================================

TEST(BindBridgeLayout, CrossPathComputeConsistency) {
    auto dxc = CreateDxc();
    ASSERT_TRUE(dxc.HasValue());

    // HLSL path
    auto hlslDesc = CompileHlslDesc(*dxc, HLSL_COMPUTE, "CSMain", ShaderStage::Compute);
    ASSERT_TRUE(hlslDesc.has_value());
    BindBridgeLayout hlslLayout(*hlslDesc);

    // SPV path
    std::pair<const char*, ShaderStage> entries[] = {{"CSMain", ShaderStage::Compute}};
    auto spvDesc = CompileSpirvDesc(*dxc, HLSL_COMPUTE, entries);
    ASSERT_TRUE(spvDesc.has_value());
    BindBridgeLayout spvLayout(*spvDesc);

    // MSL path
    auto mslRefl = CompileMslRefl(*dxc, HLSL_COMPUTE, entries);
    ASSERT_TRUE(mslRefl.has_value());
    BindBridgeLayout mslLayout(*mslRefl);

    // All three should produce non-empty bindings
    EXPECT_FALSE(hlslLayout.GetBindings().empty());
    EXPECT_FALSE(spvLayout.GetBindings().empty());
    EXPECT_FALSE(mslLayout.GetBindings().empty());

    // All three should produce valid descriptors
    auto hlslContainer = hlslLayout.GetDescriptor();
    auto spvContainer = spvLayout.GetDescriptor();
    auto mslContainer = mslLayout.GetDescriptor();

    auto hasContent = [](const RootSignatureDescriptor& d) {
        return !d.RootDescriptors.empty() || !d.DescriptorSets.empty() || d.Constant.has_value();
    };
    EXPECT_TRUE(hasContent(hlslContainer.Get()));
    EXPECT_TRUE(hasContent(spvContainer.Get()));
    EXPECT_TRUE(hasContent(mslContainer.Get()));
}

// ============================================================
// RootSignatureDescriptorContainer
// ============================================================

TEST(RootSignatureDescriptorContainer, DefaultConstruct) {
    RootSignatureDescriptorContainer container{};
    const auto& desc = container.Get();
    EXPECT_TRUE(desc.RootDescriptors.empty());
    EXPECT_TRUE(desc.DescriptorSets.empty());
    EXPECT_TRUE(desc.StaticSamplers.empty());
    EXPECT_FALSE(desc.Constant.has_value());
}

TEST(RootSignatureDescriptorContainer, CopyFromDescriptor) {
    RootSignatureDescriptor srcDesc{};
    RootSignatureRootDescriptor rd{};
    rd.Slot = 0;
    rd.Space = 0;
    rd.Type = ResourceBindType::CBuffer;
    rd.Stages = ShaderStage::Vertex;
    std::vector<RootSignatureRootDescriptor> rds = {rd};
    srcDesc.RootDescriptors = rds;

    RootSignatureDescriptorContainer container(srcDesc);
    const auto& desc = container.Get();
    ASSERT_EQ(desc.RootDescriptors.size(), 1u);
    EXPECT_EQ(desc.RootDescriptors[0].Slot, 0u);
    EXPECT_EQ(desc.RootDescriptors[0].Type, ResourceBindType::CBuffer);
}
