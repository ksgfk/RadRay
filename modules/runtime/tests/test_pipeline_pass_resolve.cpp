#include "render_test_framework.h"

#include <gtest/gtest.h>
#include <fmt/format.h>

#include <radray/guid.h>
#include <radray/runtime/pipeline_state_cache.h>
#include <radray/runtime/render_pass_registry.h>
#include <radray/runtime/shader_variant_library.h>
#include <radray/render/shader_compiler/dxc.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/render_framework/render_queue.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/shader_asset.h>

namespace radray::render::test {
namespace {

constexpr std::string_view kMeshPassSource = R"(
struct VSOut {
    float4 Pos : SV_POSITION;
    float3 Color : COLOR0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.Pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.Color = float3(uv, 0.0);
#ifdef _DOUBLE
    o.Color *= 2.0;
#endif
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET {
    return float4(i.Color, 1.0);
}
)";

class FakeProxy : public PrimitiveSceneProxy {
public:
    FakeProxy() noexcept = default;
    ~FakeProxy() noexcept override = default;
};

// 构造一个含固定图形状态 (一个 RGBA8 color target + depth) 的 mesh pass。
ShaderPassDesc MakeMeshPass() {
    ShaderPassDesc pass{};
    pass.PassTag = "ForwardLit";
    pass.Source = string{kMeshPassSource};
    pass.VertexEntry = "VSMain";
    pass.PixelEntry = "PSMain";
    pass.Primitive = PrimitiveState::Default();
    pass.MultiSample = MultiSampleState::Default();  // Count=1 (D3D12 要求)
    // Default(format) 会置 WriteMask=All (D3D12 要求非零写掩码)。
    pass.ColorTargets.push_back(ColorTargetState::Default(TextureFormat::RGBA8_UNORM));
    return pass;
}

class PipelinePassResolveTest : public ::testing::TestWithParam<TestBackend> {
protected:
    void SetUp() override {
        string reason;
        if (!_ctx.Initialize(this->GetParam(), &reason)) {
            GTEST_SKIP() << fmt::format("Init failed on {}: {}", format_as(this->GetParam()), reason);
        }
        _layoutLibrary = make_unique<PipelineLayoutLibrary>(_ctx.GetDevicePtr());
        _variantCache = CreateShaderVariantLibrary(
                            _ctx.GetDevicePtr(), _ctx.GetDxc(), _layoutLibrary.get())
                            .Release();
        _renderPassRegistry = make_unique<RenderPassRegistry>(_ctx.GetDevicePtr());
        _psoCache = make_unique<GraphicsPipelineStateLibrary>(_ctx.GetDevicePtr());
    }

    // 模拟一个 RenderPipelinePass 的核心逻辑: 对 DrawList 中每一项,
    // 解析变体 -> 构造 graphics PSO descriptor -> 走 PSO 缓存拿 PSO。
    Nullable<GraphicsPipelineState*> ResolveDrawItemPso(const DrawItem& item, const ShaderPassDesc& passDesc) {
        auto variant = item.Material->ResolveVariant(*_variantCache, item.PassIndex);
        if (!variant.HasValue()) {
            return nullptr;
        }
        GraphicsPipelineStateDescriptor desc{};
        desc.PipelineLayout = variant.Get()->Layout;
        desc.VS = ShaderEntry{.Target = variant.Get()->VS, .EntryPoint = passDesc.VertexEntry};
        desc.PS = ShaderEntry{.Target = variant.Get()->PS, .EntryPoint = passDesc.PixelEntry};
        desc.Primitive = passDesc.Primitive;
        desc.DepthStencil = passDesc.DepthStencil;
        desc.MultiSample = passDesc.MultiSample;
        desc.ColorTargets = passDesc.ColorTargets;
        vector<RenderPassColorAttachmentDescriptor> colorAttachments;
        colorAttachments.reserve(passDesc.ColorTargets.size());
        for (const ColorTargetState& target : passDesc.ColorTargets) {
            colorAttachments.push_back(RenderPassColorAttachmentDescriptor{
                .Format = target.Format,
                .SampleCount = passDesc.MultiSample.Count,
                .Load = LoadAction::DontCare,
                .Store = StoreAction::Store});
        }
        std::optional<RenderPassDepthStencilAttachmentDescriptor> depthAttachment;
        if (passDesc.DepthStencil.has_value()) {
            depthAttachment = RenderPassDepthStencilAttachmentDescriptor{
                .Format = passDesc.DepthStencil->Format,
                .SampleCount = passDesc.MultiSample.Count,
                .DepthLoad = LoadAction::DontCare,
                .DepthStore = StoreAction::Store,
                .StencilLoad = LoadAction::DontCare,
                .StencilStore = StoreAction::Discard};
        }
        auto renderPass = _renderPassRegistry->GetOrCreateRenderPass(RenderPassDescriptor{
            .ColorAttachments = colorAttachments,
            .DepthStencilAttachment = depthAttachment});
        if (!renderPass.HasValue()) {
            return nullptr;
        }
        desc.CompatibleRenderPass = renderPass.Get();
        return _psoCache->GetOrCreate(desc);
    }

    ComputeTestContext _ctx{};
    unique_ptr<PipelineLayoutLibrary> _layoutLibrary{};
    unique_ptr<ShaderVariantLibrary> _variantCache{};
    unique_ptr<RenderPassRegistry> _renderPassRegistry{};
    unique_ptr<GraphicsPipelineStateLibrary> _psoCache{};
};

TEST_P(PipelinePassResolveTest, DrawListItemResolvesToPso) {
    ShaderPassDesc meshPass = MakeMeshPass();
    ShaderKeywordSet kw;
    kw.Add("_DOUBLE");
    vector<ShaderPassDesc> passes;
    passes.push_back(meshPass);
    AssetManager mgr;
    auto shaderRef = mgr.AddReady<ShaderAsset>(Guid::NewGuid(),
        std::make_unique<ShaderAsset>(std::move(kw), std::move(passes)));

    MaterialAsset material{shaderRef};
    FakeProxy proxy;

    DrawList list;
    ASSERT_TRUE(list.AddPrimitive(material.CreateSnapshot(), &proxy, "ForwardLit", 0, 3.0f));
    ASSERT_EQ(list.Size(), 1u);
    list.SortOpaque();

    _ctx.ClearCapturedErrors();
    auto pso = ResolveDrawItemPso(list.Items()[0], meshPass);
    ASSERT_TRUE(pso.HasValue()) << _ctx.JoinCapturedErrors();
    EXPECT_EQ(_psoCache->Count(), 1u);
}

TEST_P(PipelinePassResolveTest, SameMaterialStateHitsPsoCache) {
    ShaderPassDesc meshPass = MakeMeshPass();
    ShaderKeywordSet kw;
    kw.Add("_DOUBLE");
    vector<ShaderPassDesc> passes;
    passes.push_back(meshPass);
    AssetManager mgr;
    auto shaderRef = mgr.AddReady<ShaderAsset>(Guid::NewGuid(),
        std::make_unique<ShaderAsset>(std::move(kw), std::move(passes)));

    MaterialAsset matA{shaderRef};
    MaterialAsset matB{shaderRef};  // 相同 shader/state/keyword -> 应共享变体与 PSO
    FakeProxy proxy;

    DrawList list;
    list.AddPrimitive(matA.CreateSnapshot(), &proxy, "ForwardLit", 0, 1.0f);
    list.AddPrimitive(matB.CreateSnapshot(), &proxy, "ForwardLit", 0, 2.0f);

    _ctx.ClearCapturedErrors();
    auto psoA = ResolveDrawItemPso(list.Items()[0], meshPass);
    auto psoB = ResolveDrawItemPso(list.Items()[1], meshPass);
    ASSERT_TRUE(psoA.HasValue()) << _ctx.JoinCapturedErrors();
    ASSERT_TRUE(psoB.HasValue());
    // 相同变体 (相同 shader ProgramId + pass + 空 keyword) + 相同渲染状态 -> 命中同一 PSO。
    EXPECT_EQ(psoA.Get(), psoB.Get());
    EXPECT_EQ(_variantCache->Count(), 1u);
    EXPECT_EQ(_psoCache->Count(), 1u);
}

TEST_P(PipelinePassResolveTest, DifferentKeywordProducesDifferentPso) {
    ShaderPassDesc meshPass = MakeMeshPass();
    ShaderKeywordSet kw;
    kw.Add("_DOUBLE");
    vector<ShaderPassDesc> passes;
    passes.push_back(meshPass);
    AssetManager mgr;
    auto shaderRef = mgr.AddReady<ShaderAsset>(Guid::NewGuid(),
        std::make_unique<ShaderAsset>(std::move(kw), std::move(passes)));

    MaterialAsset plain{shaderRef};
    MaterialAsset doubled{shaderRef};
    doubled.EnableKeyword("_DOUBLE");
    FakeProxy proxy;

    DrawList list;
    list.AddPrimitive(plain.CreateSnapshot(), &proxy, "ForwardLit");
    list.AddPrimitive(doubled.CreateSnapshot(), &proxy, "ForwardLit");

    _ctx.ClearCapturedErrors();
    auto psoPlain = ResolveDrawItemPso(list.Items()[0], meshPass);
    auto psoDoubled = ResolveDrawItemPso(list.Items()[1], meshPass);
    ASSERT_TRUE(psoPlain.HasValue()) << _ctx.JoinCapturedErrors();
    ASSERT_TRUE(psoDoubled.HasValue());
    // 不同 keyword -> 不同变体 -> 不同 PSO。
    EXPECT_NE(psoPlain.Get(), psoDoubled.Get());
    EXPECT_EQ(_variantCache->Count(), 2u);
    EXPECT_EQ(_psoCache->Count(), 2u);
}

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    PipelinePassResolveTest,
    ::testing::ValuesIn(GetEnabledTestBackends()),
    [](const ::testing::TestParamInfo<TestBackend>& info) {
        return string{fmt::format("{}", info.param)};
    });

}  // namespace
}  // namespace radray::render::test
