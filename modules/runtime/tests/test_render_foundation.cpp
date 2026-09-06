#include "gpu_test_fixture.h"

#include <gtest/gtest.h>
#include <radray/runtime/render_framework/render_resource_pool.h>
#include <radray/runtime/render_framework/render_workload.h>

namespace radray {
namespace {

render::TextureDescriptor ColorDesc(uint32_t width = 64) {
    return {render::TextureDimension::Dim2D, width, 32, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource, {}};
}

TEST(RenderWorkloadTest, IdsAreNotReusedAndPlanResetRemovesPriorFamilies) {
    RenderOutputRegistry registry;
    const auto first = registry.RegisterPresentation("first", ColorDesc());
    ASSERT_TRUE(registry.Unregister(first));
    const auto second = registry.RegisterPresentation("second", ColorDesc());
    EXPECT_GT(second.Value, first.Value);
    EXPECT_GT(AllocateViewStateId().Value, 0u);
    auto outputs = registry.GetGameThreadInfos();
    RenderFramePlan plan;
    RenderWorkloadBuilder builder(plan, outputs);
    EXPECT_TRUE(builder.AddViewFamily({"family", second}));
    EXPECT_FALSE(builder.AddViewFamily({"duplicate", second}));
    EXPECT_NE(plan.Diagnostics.back().find("family"), string::npos);
    plan.Reset();
    EXPECT_TRUE(plan.ViewFamilies.empty());
    EXPECT_TRUE(plan.Diagnostics.empty());
    EXPECT_FALSE(builder.AddViewFamily({"stale", first}));
}

TEST(RenderWorkloadTest, ResolvesSubrectAspectExplicitProjectionAndJitter) {
    const RenderOutputInfo output{{1}, RenderOutputKind::ExternalColorTexture, "output", 801, 601, render::TextureFormat::RGBA8_UNORM, 1, true};
    RenderViewDesc view;
    view.ViewRect = {0, 0, .5f, 1};
    view.JitterPixels = {.5f, .25f};
    RenderViewFamilyDesc family{"family", output.Id, 1, {view}};
    string reason;
    auto resolved = ResolveRenderViewFamily(family, output, 7, 16384, reason);
    ASSERT_TRUE(resolved) << reason;
    const auto& actual = resolved->Views.front();
    EXPECT_EQ(actual.ViewRect.Width, 401u);
    EXPECT_NEAR(actual.Projection(1, 1) / actual.Projection(0, 0), 401.0f / 601, 1e-6f);
    EXPECT_NEAR(actual.JitterNdc.x(), 1.0f / 401, 1e-7f);
    EXPECT_NEAR(actual.JitterNdc.y(), -.5f / 601, 1e-7f);
    family.Views[0].Projection = ExplicitProjectionDesc{Eigen::Matrix4f::Identity()};
    family.Views[0].JitterPixels.setZero();
    resolved = ResolveRenderViewFamily(family, output, 7, 16384, reason);
    ASSERT_TRUE(resolved);
    EXPECT_TRUE(resolved->Views[0].Projection.isIdentity());
    family.Views[0].ViewRect.Width = 1.1f;
    EXPECT_FALSE(ResolveRenderViewFamily(family, output, 7, 16384, reason));
}

TEST(RenderWorkloadTest, MutationRequiresRenderIdle) {
    RenderOutputRegistry registry;
    const auto id = registry.RegisterPresentation("idle", ColorDesc());
    registry.SetRenderIdle(false);
#if RADRAY_IS_DEBUG
    EXPECT_DEATH(registry.Unregister(id), "");
#endif
    registry.SetRenderIdle(true);
    EXPECT_TRUE(registry.Unregister(id));
}

class RenderFoundationTest : public testing::TestWithParam<render::RenderBackend> {};

TEST_P(RenderFoundationTest, ExternalOutputsAndRelativeExtentsAreValidated) {
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(GetParam(), context, true)) {
        if (render::test::SetupMustFail(context.Status, render::test::RequiredBackend(GetParam()))) FAIL() << context.Reason;
        GTEST_SKIP() << context.Reason;
    }
    auto target = render::test::MakeRenderTarget(context.Device.get(), render::TextureFormat::RGBA8_UNORM, 64, 32,
                                                 render::TextureUse::RenderTarget | render::TextureUse::Resource);
    ASSERT_TRUE(target);
    RenderOutputRegistry registry;
    ExternalRenderOutputDesc external{"test", target->Tex.get(), target->View.get()};
    auto id = registry.RegisterExternal(external);
    ASSERT_TRUE(id.IsValid());
    auto surface = registry.ResolveExternal(id);
    ASSERT_TRUE(surface);
    EXPECT_FALSE(surface->PreserveContents);
    surface->CurrentState = render::TextureState::ShaderRead;
    surface->Written = true;
    registry.CommitExternalState(*surface);
    EXPECT_EQ(registry.ResolveExternal(id)->CurrentState, render::TextureState::ShaderRead);
    EXPECT_TRUE(registry.UpdateExternalOutputState(id, render::TextureState::RenderTarget, true));
    EXPECT_TRUE(registry.ResolveExternal(id)->PreserveContents);
    external.ColorAttachmentView = nullptr;
    EXPECT_FALSE(registry.RegisterExternal(external).IsValid());
    ResolvedRenderViewFamily family{};
    family.RenderSize = {101, 57};
    family.OutputSize = {640, 360};
    RuntimeTextureDesc desc;
    desc.Format = render::TextureFormat::RGBA8_UNORM;
    desc.Usage = render::TextureUse::RenderTarget;
    desc.Extent = {.Mode = RenderExtentMode::RelativeToFamilyRenderExtent, .ScaleX = .5f, .ScaleY = .5f, .AlignX = 8, .AlignY = 8};
    string reason;
    auto native = ResolveRuntimeTextureDesc(desc, family, *context.Device, reason);
    ASSERT_TRUE(native) << reason;
    EXPECT_EQ(native->Width, 56u);
    EXPECT_EQ(native->Height, 32u);
    desc.Extent.AlignX = 0;
    EXPECT_FALSE(ResolveRuntimeTextureDesc(desc, family, *context.Device, reason));
    EXPECT_EQ(context.ValidationErrors.load(), 0u);
}

TEST_P(RenderFoundationTest, PoolKeepsExactKeysPhysicalStatesAndSafeViewLifetime) {
    render::test::DeviceContext context;
    if (!render::test::TryCreateDevice(GetParam(), context, true)) {
        if (render::test::SetupMustFail(context.Status, render::test::RequiredBackend(GetParam()))) FAIL() << context.Reason;
        GTEST_SKIP() << context.Reason;
    }
    render::RenderPassRegistry registry(context.Device.get());
    RenderResourcePool pool(*context.Device, registry, 2), other(*context.Device, registry, 2);
    pool.BeginFlight(1);
    other.BeginFlight(1);
    auto a = pool.AcquireTexture(ColorDesc(), "a");
    auto b = pool.AcquireTexture(ColorDesc(), "b");
    auto c = other.AcquireTexture(ColorDesc(), "other flight");
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    ASSERT_TRUE(c);
    EXPECT_NE(a->Texture.get(), b->Texture.get());
    EXPECT_NE(a->Texture.get(), c->Texture.get());
    auto view = pool.GetTextureView(*a, {render::TextureDimension::Dim2D, ColorDesc().Format, {0, 1, 0, 1}, render::TextureViewUsage::RenderTarget});
    ASSERT_TRUE(view);
    const render::RenderPassColorAttachmentDescriptor attachment{ColorDesc().Format, 1, render::LoadAction::Clear, render::StoreAction::Store};
    auto pass = registry.GetOrCreateRenderPass({std::span{&attachment, 1}, {}});
    ASSERT_TRUE(pass);
    render::TextureView* color = view.Get();
    auto framebuffer = registry.GetOrCreateFramebuffer({pass.Get(), std::span{&color, 1}, nullptr, 64, 32, 1});
    ASSERT_TRUE(framebuffer);
    a->States[0] = render::TextureState::ShaderRead;
    auto* first = a.Get();
    pool.EndGraph();
    other.EndGraph();
    auto sameCycle = pool.AcquireTexture(ColorDesc(), "same cycle");
    ASSERT_TRUE(sameCycle);
    EXPECT_NE(first, sameCycle.Get());
    pool.EndGraph();
    pool.BeginFlight(3);
    a = pool.AcquireTexture(ColorDesc(), "reuse");
    EXPECT_EQ(a.Get(), first);
    EXPECT_EQ(a->States[0], render::TextureState::ShaderRead);
    EXPECT_EQ(pool.GetStats().Hits, 1u);
    pool.EndGraph();
    for (uint64_t frame = 5; frame < 105; ++frame) {
        pool.BeginFlight(frame);
        ASSERT_TRUE(pool.AcquireTexture(ColorDesc(frame % 2 ? 64 : 128), "resizing"));
        pool.EndGraph();
    }
    EXPECT_LE(pool.GetStats().TextureCount, 2u);
    pool.BeginFlight(105);
    pool.BeginFlight(106);
    pool.BeginFlight(107);
    EXPECT_EQ(pool.GetStats().TextureCount, 0u);
    EXPECT_EQ(registry.GetFramebufferCount(), 0u);
    EXPECT_EQ(context.ValidationErrors.load(), 0u);
}

INSTANTIATE_TEST_SUITE_P(Backends, RenderFoundationTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
