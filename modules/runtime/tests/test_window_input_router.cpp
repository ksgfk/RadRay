#include <gtest/gtest.h>
#include <radray/runtime/window_input_router.h>
#include <radray/runtime/render_framework/render_workload.h>

namespace radray {
TEST(WindowInputRouterTest, ReleaseBelongsToOriginalPressAcrossCaptureChanges) {
    WindowInputRouter router;
    vector<WindowInputEvent> events;
    auto connection = router.EventInput().connect([&](const auto& event) { events.push_back(event); });
    router.Push({.Type = WindowInputType::Key, .Key = KeyCode::W, .State = Action::PRESSED});
    router.Dispatch();
    router.SetCapture(true, true);
    router.Push({.Type = WindowInputType::Key, .Key = KeyCode::W, .State = Action::RELEASED});
    router.Dispatch();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events.back().State, Action::RELEASED);
    router.Push({.Type = WindowInputType::Key, .Key = KeyCode::A, .State = Action::PRESSED});
    router.Dispatch();
    router.SetCapture(false, false);
    router.Push({.Type = WindowInputType::Key, .Key = KeyCode::A, .State = Action::RELEASED});
    router.Dispatch();
    EXPECT_EQ(events.size(), 2u);
}
TEST(WindowInputRouterTest, FocusAndCaptureLossCancelHeldInputExactlyOnce) {
    WindowInputRouter router;
    vector<WindowInputEvent> events;
    auto connection = router.EventInput().connect([&](const auto& event) { events.push_back(event); });
    router.Push({.Type = WindowInputType::Key, .Key = KeyCode::LEFT_SHIFT, .State = Action::PRESSED});
    router.Push({.Type = WindowInputType::Button, .Button = MouseButton::BUTTON_RIGHT, .State = Action::PRESSED});
    router.Dispatch();
    router.Push({.Type = WindowInputType::CaptureLost});
    router.Dispatch();
    router.Push({.Type = WindowInputType::Focus, .Focused = false});
    router.Dispatch();
    router.Cancel();
    uint32_t releases = 0;
    for (const auto& event : events) releases += event.State == Action::RELEASED;
    EXPECT_EQ(releases, 2u);
}
TEST(WindowInputRouterTest, OwnsUtf8AndFloatingPointScrollAndSuppressesAuxiliaryInput) {
    WindowInputRouter router;
    vector<WindowInputEvent> events;
    auto connection = router.EventInput().connect([&](const auto& event) { events.push_back(event); });
    router.Push({.Type = WindowInputType::Text, .Text = "\xe4\xb8\xad\xf0\x9f\x8e\xa8"});
    router.Push({.Type = WindowInputType::Scroll, .Position = {-.25f, 1.5f}});
    router.Dispatch();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].Text.size(), 7u);
    EXPECT_EQ(events[1].Position.x(), -.25f);
    router.SetApplicationEnabled(false);
    router.Push({.Type = WindowInputType::Text, .Text = "tool"});
    router.Push({.Type = WindowInputType::Key, .Key = KeyCode::W, .State = Action::PRESSED});
    router.Dispatch();
    EXPECT_EQ(events.size(), 2u);
}
TEST(RenderWorkloadTest, IndependentOutputsDeduplicateWithSceneFamilies) {
    RenderOutputRegistry registry;
    const render::TextureDescriptor desc{render::TextureDimension::Dim2D, 32, 32, 1, 1, 1, render::TextureFormat::RGBA8_UNORM,
                                         render::MemoryType::Device, render::TextureUse::RenderTarget};
    const auto main = registry.RegisterPresentation("main", desc);
    const auto tool = registry.RegisterPresentation("tool", desc, RenderOutputUsage::Auxiliary);
    auto infos = registry.GetGameThreadInfos();
    RenderFramePlan plan;
    RenderWorkloadBuilder builder(plan, infos);
    EXPECT_TRUE(builder.RequestOutput(main));
    EXPECT_TRUE(builder.RequestOutput(main));
    EXPECT_TRUE(builder.AddViewFamily({"scene", main}));
    EXPECT_TRUE(builder.RequestOutput(tool));
    EXPECT_EQ(plan.Outputs.size(), 2u);
    EXPECT_EQ(plan.ViewFamilies.size(), 1u);
    EXPECT_EQ(registry.Find(tool)->Usage, RenderOutputUsage::Auxiliary);
    EXPECT_FALSE(builder.RequestOutput({UINT64_MAX}));
    plan.Reset();
    EXPECT_TRUE(plan.Outputs.empty());
}
}  // namespace radray
