#include "test_scene_views.h"
#include <gtest/gtest.h>
#include <radray/runtime/application.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/world_manager.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/components/camera_component.h>

namespace radray::test {

class TickCamera final : public CameraComponent {
public:
    TickCamera() { SetTickEnabled(true); }
    void TickComponent(float) override { SetRelativeLocation({42 + float(++Ticks), 3, 7}); }
    uint32_t Ticks{0};
};

class ViewCollectionApp final : public Application {
public:
    int Violation{0};
    uint32_t Collected{0};

protected:
    void OnInit() override {
        const auto id = GetWorldManager()->CreateWorld();
        auto world = GetWorldManager()->GetWorld(id);
        GetWorldManager()->RequestRenderConnection(id, true);
        _camera = world->SpawnActor()->AddComponent<TickCamera>();
        auto* parent = world->SpawnActor()->AddComponent<SceneComponent>();
        parent->SetRelativeLocation({10, 0, 0});
        _camera->RequestReparent(parent);
    }
    void OnUpdate(const AppUpdateContext& context) override {
        _flight = context.FlightIndex;
        _camera->SetRelativeLocation({1, 0, 0});
        if (Collected == 4) RequestExit();
    }
    void OnCollectRenderViews(SceneViewCollector& collector) override {
        if (Violation == 1) _camera->SetPerspective(1, 0.1f, 10);
        if (Violation == 2) _camera->SetRelativeLocation({});
        if (Violation == 3) GetScheduler().Pump();
        if (Violation == 4) GetWorldManager()->CreateWorld();
        auto* renderer = GetRenderSystem().Get();
        EXPECT_TRUE(renderer->GetFrameViewsRT(_flight).empty());
        ASSERT_TRUE(collector.Add(*_camera.Get()));
        auto views = renderer->GetFrameViewsRT(_flight);
        ASSERT_EQ(views.size(), 1u);
        EXPECT_FLOAT_EQ(views[0].View(0, 3), -52 - float(_camera->Ticks));
        EXPECT_FLOAT_EQ(views[0].View(1, 3), -3);
        EXPECT_FLOAT_EQ(views[0].View(2, 3), -7);
        SceneViewRequest copy = views[0];
        ASSERT_TRUE(collector.Add(copy));
        copy.View.setZero();
        EXPECT_FLOAT_EQ(renderer->GetFrameViewsRT(_flight)[1].View(0, 3), -52 - float(_camera->Ticks));
        ++Collected;
    }

private:
    Nullable<TickCamera*> _camera{nullptr};
    uint32_t _flight{0};
};

uint32_t RunViewCollection(int violation) {
    ViewCollectionApp app;
    app.Violation = violation;
    EXPECT_EQ(app.Run({.Backend = render::RenderBackend::D3D12, .FlightDataCount = 3, .Systems = ApplicationSystem::World | ApplicationSystem::Render}), 0);
    return app.Collected;
}

TEST(SceneViews, FinalTickAndS1CameraValuesAreCapturedByValue) {
    EXPECT_GE(RunViewCollection(), 4u);
}
TEST(SceneViews, ViewportAndProjectionAreBackendIndependent) {
    SceneViewRequest request{.Scene = {0, 0}, .Viewport = {0.25f, 0.25f, 0.5f, 0.5f}};
    auto d3 = ResolveSceneView(request, 100, 80, render::RenderBackend::D3D12);
    auto vk = ResolveSceneView(request, 100, 80, render::RenderBackend::Vulkan);
    ASSERT_TRUE(d3);
    ASSERT_TRUE(vk);
    EXPECT_TRUE(d3->ViewProjection.isApprox(vk->ViewProjection));
    EXPECT_EQ(d3->Scissor.X, 25);
    EXPECT_EQ(d3->Scissor.Y, 20);
    EXPECT_FLOAT_EQ(d3->Viewport.Height, 40);
    EXPECT_FLOAT_EQ(vk->Viewport.Height, -40);
    EXPECT_FLOAT_EQ(vk->Viewport.Y, 60);
    EXPECT_FALSE(ResolveSceneView(request, 0, 0, render::RenderBackend::D3D12));
    request.NearZ = -1;
    EXPECT_FALSE(IsValidSceneView(request));
}
TEST(SceneViewsDeathTest, CollectionRejectsCameraWorldAndSchedulerMutation) {
    for (int violation = 1; violation <= 4; ++violation) {
        EXPECT_DEATH(RunViewCollection(violation), "");
    }
}

}  // namespace radray::test
