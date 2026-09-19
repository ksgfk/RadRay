#include "scene_test_support.h"
#include "runtime_test_support.h"
#include "gpu_test_fixture.h"

#include <semaphore>

#include <radray/scope_guard.h>
#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/world_manager.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>

namespace radray {
namespace {

TEST(SceneDelivery, EmptyBatchesAcrossFlightsWithoutDrawing) {
    for (uint32_t count : {1u, 2u, 3u}) {
        SCOPED_TRACE(count);
        Application app;
        RenderSystem render{&app, count};
        World world{&app};
        const auto sceneId = world.AttachToRendering(render);
        Nullable<const RenderScene*> scene{nullptr};
        for (uint32_t round = 0; round < 8; ++round) {
            for (uint32_t flight = 0; flight < count; ++flight) {
                test::PrepareScene(world, render, flight);
            }
            for (uint32_t flight = 0; flight < count; ++flight) {
                render.ConsumeRenderUpdates(flight);
                if (!scene) scene = render.GetSceneRT(sceneId);
                EXPECT_EQ(render.GetSceneRT(sceneId), scene);
                render.OnFlightCompletedGT({.FlightIndex = flight, .GpuWorkCompleted = false});
            }
        }
    }
}

TEST(SceneDelivery, AbandonDoesNotPublishOrRequireCompletion) {
    for (bool abandonAll : {false, true}) {
        Application app;
        RenderSystem render{&app, 3};
        World world{&app};
        const auto sceneId = world.AttachToRendering(render);
        const PrimitiveId id = world.SpawnActor()->AddComponent<PrimitiveComponent>()->GetPrimitiveId();
        test::PrepareScene(world, render, 2);
        ASSERT_EQ(test::SceneBatch(render, sceneId, 2).CreatePrimitives, vector<PrimitiveId>{id});
        if (abandonAll)
            render.AbandonUnpublishedFramesGT();
        else
            render.AbandonUnpublishedFrameGT(2);
        EXPECT_TRUE(render.GetFrameUpdatesRT(2).empty());
        EXPECT_FALSE(render.GetSceneRT(sceneId));
        world.DetachFromRendering();
        render.OnShutdown();
        world.DetachFromRendering();
        render.OnShutdown();
    }
}

TEST(SceneDelivery, PrimitiveUpdatesAcrossFlightsWithoutDrawing) {
    for (uint32_t count : {1u, 2u, 3u}) {
        SCOPED_TRACE(count);
        Application app;
        RenderSystem render{&app, count};
        World world{&app};
        const auto sceneId = world.AttachToRendering(render);
        auto* actor = world.SpawnActor();
        Nullable<PrimitiveComponent*> component{nullptr};
        vector<PrimitiveId> expected(count);
        for (uint32_t round = 0; round < 4; ++round) {
            for (uint32_t flight = 0; flight < count; ++flight) {
                if (component) actor->RemoveComponent(component.Get());
                component = actor->AddComponent<PrimitiveComponent>();
                expected[flight] = component->GetPrimitiveId();
                test::PrepareScene(world, render, flight);
            }
            for (uint32_t flight = 0; flight < count; ++flight) {
                render.ConsumeRenderUpdates(flight);
                EXPECT_TRUE(render.GetSceneRT(sceneId)->ContainsPrimitive(expected[flight]));
                if (flight > 0) EXPECT_FALSE(render.GetSceneRT(sceneId)->ContainsPrimitive(expected[flight - 1]));
                render.OnFlightCompletedGT({.FlightIndex = flight, .GpuWorkCompleted = false});
            }
        }
    }
}

TEST(SceneDeliveryDeathTest, RejectsInvalidFlightIndices) {
    Application app;
    World world{&app};
    RenderSystem render{&app, 1};
    EXPECT_DEATH(render.GetFrameUpdatesRT(1), "");
    EXPECT_DEATH(test::PrepareScene(world, render, 1), "");
    EXPECT_DEATH(render.ConsumeRenderUpdates(1), "");
    EXPECT_DEATH(render.OnFlightCompletedGT({.FlightIndex = 1}), "");
    EXPECT_DEATH(render.AbandonUnpublishedFrameGT(1), "");
}

#if defined(_WIN32)
class SceneDeliveryApp final : public Application {
public:
    explicit SceneDeliveryApp(bool drainOnExit) : _drainOnExit(drainOnExit) {}
    uint64_t PublishedFrames{0};
    uint64_t Completed{0};
    uint64_t Dropped{0};
    uint64_t Recorded{0};

protected:
    void OnInit() override {
        _worldId = GetWorldManager()->CreateWorld();
        _sceneId = GetWorldManager()->AttachWorldToRendering(_worldId);
        _actor = GetWorldManager()->GetWorld(_worldId)->SpawnActor();
        _component = _actor->AddComponent<PrimitiveComponent>();
        _initialId = _component->GetPrimitiveId();
        _latestId = _initialId;
    }

    void OnUpdate(const AppUpdateContext&) override {
        ++_updates;
        const auto count = GetGpuSystem()->GetFlightDataCount();
        const bool exit = _drainOnExit ? _updates == count : _updates == 13;
        if (!exit) {
            if (_updates == 2) {
                _actor->RemoveComponent(_component.Get());
                _component = _actor->AddComponent<PrimitiveComponent>();
                _latestId = _component->GetPrimitiveId();
            }
            return;
        }
        if (_drainOnExit && count > 1) {
            // Runner closes window operations after setting its exit flag, before joining RT.
            _tasks.Spawn(ReleaseRenderOnCancellation());
        }
        auto* native = GetWindowManager()->GetMainWindow()->GetNativeWindow();
        ::SendMessageW(static_cast<HWND>(native->GetNativeHandler()), WM_CLOSE, 0, 0);
    }

    void OnRender(AppFrameContext& ctx) override {
        ++Recorded;
        if (ctx.FrameSerial() == 1) EXPECT_TRUE(GetRenderSystem()->GetSceneRT(_sceneId)->ContainsPrimitive(_initialId));
        if (_drainOnExit && ctx.FrameSerial() == 1) {
            EXPECT_TRUE(_renderGate.try_acquire_for(std::chrono::seconds{10}));
        }
    }

    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        EXPECT_EQ(completion.FrameSerial, ++Completed);
        if (!completion.GpuWorkCompleted) ++Dropped;
    }

    void OnShutdown() override {
        PublishedFrames = GetGpuSystem()->GetFrameIndex();
        EXPECT_EQ(PublishedFrames, _updates - 1);
        EXPECT_EQ(Completed, PublishedFrames);
        const auto scene = GetRenderSystem()->GetSceneRT(_sceneId);
        EXPECT_EQ(scene && scene->ContainsPrimitive(_latestId), PublishedFrames > 0);
        if (scene && _latestId != _initialId) EXPECT_FALSE(scene->ContainsPrimitive(_initialId));
    }

private:
    task<void> ReleaseRenderOnCancellation() {
        auto release = MakeScopeGuard([this]() noexcept { _renderGate.release(); });
        co_await GetWindowManager()->SetSize(GetWindowManager()->GetMainWindow()->GetHandle(), 90, 70);
        ADD_FAILURE() << "Exit must cancel the unpublished window operation";
    }

    bool _drainOnExit;
    Nullable<Actor*> _actor{nullptr};
    Nullable<PrimitiveComponent*> _component{nullptr};
    WorldId _worldId;
    SceneId _sceneId;
    PrimitiveId _initialId;
    PrimitiveId _latestId;
    uint64_t _updates{0};
    std::binary_semaphore _renderGate{0};
    TaskScope _tasks;
};

void RunSceneDelivery(render::RenderBackend backend, bool threaded, bool drainOnExit) {
    {
        render::test::DeviceContext probe;
        if (!render::test::TryCreateDevice(backend, probe)) GTEST_SKIP() << probe.Reason;
    }
    for (uint32_t count : {1u, 2u, 3u}) {
        SCOPED_TRACE(count);
        test::RuntimeLogCapture logs;
        SceneDeliveryApp app{drainOnExit};
        ASSERT_EQ(app.Run({.Backend = backend, .EnableValidation = true, .Multithreaded = threaded, .EnableSynchronizationValidation = true, .WindowTitle = "RenderScene delivery", .WindowWidth = 80, .WindowHeight = 60, .FlightDataCount = count, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}), 0);
        EXPECT_EQ(app.PublishedFrames, drainOnExit ? count - 1 : 12u);
        if (drainOnExit && count == 3) EXPECT_GE(app.Dropped, 1u);
        if (!drainOnExit) EXPECT_EQ(app.Recorded + app.Dropped, app.PublishedFrames);
        EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    }
}

class MultiWorldDeliveryApp final : public Application {
public:
    uint64_t Completed{0};

protected:
    void OnInit() override {
        _firstWorld = GetWorldManager()->CreateWorld();
        _firstScene = GetWorldManager()->AttachWorldToRendering(_firstWorld);
        _mesh = GetWorldManager()->GetWorld(_firstWorld)->SpawnActor()->AddComponent<StaticMeshComponent>();
        GetWorldManager()->GetWorld(_firstWorld)->SetTickEnabled(false);
        _secondWorld = GetWorldManager()->CreateWorld();
        _secondScene = GetWorldManager()->AttachWorldToRendering(_secondWorld);
        GetWorldManager()->GetWorld(_secondWorld)->SpawnActor()->AddComponent<StaticMeshComponent>();
    }

    void OnUpdate(const AppUpdateContext&) override {
        ++_updates;
        _mesh->SetRelativeLocation({static_cast<float>(_updates), 0, 0});
        if (_updates == 2) {
            GetWorldManager()->DestroyWorld(_secondWorld);
            EXPECT_FALSE(GetWorldManager()->GetWorld(_secondWorld));
        }
        if (_updates == 3) {
            const auto replacement = GetWorldManager()->CreateWorld();
            EXPECT_EQ(replacement.Index, _secondWorld.Index);
            EXPECT_GT(replacement.Generation, _secondWorld.Generation);
            _replacementScene = GetWorldManager()->AttachWorldToRendering(replacement);
            GetWorldManager()->GetWorld(replacement)->SpawnActor()->AddComponent<StaticMeshComponent>();
        }
        if (_updates == 5) {
            GetWorldManager()->DetachWorldFromRendering(_firstWorld);
            _reattachedScene = GetWorldManager()->AttachWorldToRendering(_firstWorld);
        }
        if (_updates == 9) {
            auto* native = GetWindowManager()->GetMainWindow()->GetNativeWindow();
            ::SendMessageW(static_cast<HWND>(native->GetNativeHandler()), WM_CLOSE, 0, 0);
        }
    }

    void OnRender(AppFrameContext& ctx) override {
        const auto frame = ctx.FrameSerial();
        const auto scene = GetRenderSystem()->GetSceneRT(frame < 5 ? _firstScene : _reattachedScene);
        ASSERT_TRUE(scene);
        ASSERT_EQ(scene->GetStaticMeshes().size(), 1u);
        EXPECT_FLOAT_EQ(scene->GetStaticMesh(scene->GetStaticMeshes()[0])->LocalToWorld(0, 3), static_cast<float>(frame));
        EXPECT_EQ(static_cast<bool>(GetRenderSystem()->GetSceneRT(_secondScene)), frame < 2);
        if (frame >= 3) EXPECT_TRUE(GetRenderSystem()->GetSceneRT(_replacementScene));
        if (frame >= 5) EXPECT_FALSE(GetRenderSystem()->GetSceneRT(_firstScene));
    }

    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        EXPECT_EQ(completion.FrameSerial, ++Completed);
    }

    void OnShutdown() override {
        EXPECT_EQ(Completed, 8u);
        EXPECT_FALSE(GetRenderSystem()->GetSceneRT(_firstScene));
        EXPECT_FALSE(GetRenderSystem()->GetSceneRT(_secondScene));
        EXPECT_TRUE(GetRenderSystem()->GetSceneRT(_replacementScene));
        const auto scene = GetRenderSystem()->GetSceneRT(_reattachedScene);
        ASSERT_TRUE(scene);
        EXPECT_FLOAT_EQ(scene->GetStaticMesh(scene->GetStaticMeshes()[0])->LocalToWorld(0, 3), 8);
    }

private:
    WorldId _firstWorld;
    WorldId _secondWorld;
    SceneId _firstScene;
    SceneId _secondScene;
    SceneId _replacementScene;
    SceneId _reattachedScene;
    Nullable<StaticMeshComponent*> _mesh{nullptr};
    uint32_t _updates{0};
};

void RunMultiWorldDelivery(render::RenderBackend backend, bool threaded) {
    {
        render::test::DeviceContext probe;
        if (!render::test::TryCreateDevice(backend, probe)) GTEST_SKIP() << probe.Reason;
    }
    for (uint32_t count : {1u, 2u, 3u}) {
        SCOPED_TRACE(count);
        test::RuntimeLogCapture logs;
        MultiWorldDeliveryApp app;
        ASSERT_EQ(app.Run({.Backend = backend, .EnableValidation = true, .Multithreaded = threaded, .EnableSynchronizationValidation = true, .WindowTitle = "Multiple world delivery", .WindowWidth = 80, .WindowHeight = 60, .FlightDataCount = count, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}), 0);
        EXPECT_EQ(app.Completed, 8u);
        EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    }
}

TEST(MultiWorldSceneRunner, D3D12SingleThreadFlights) { RunMultiWorldDelivery(render::RenderBackend::D3D12, false); }
TEST(MultiWorldSceneRunner, D3D12ThreadedFlights) { RunMultiWorldDelivery(render::RenderBackend::D3D12, true); }
TEST(MultiWorldSceneRunner, VulkanSingleThreadFlights) { RunMultiWorldDelivery(render::RenderBackend::Vulkan, false); }
TEST(MultiWorldSceneRunner, VulkanThreadedFlights) { RunMultiWorldDelivery(render::RenderBackend::Vulkan, true); }

TEST(SceneDeliveryRunner, D3D12SingleThreadFlights) { RunSceneDelivery(render::RenderBackend::D3D12, false, false); }
TEST(SceneDeliveryRunner, D3D12ThreadedFlights) { RunSceneDelivery(render::RenderBackend::D3D12, true, false); }
TEST(SceneDeliveryRunner, VulkanSingleThreadFlights) { RunSceneDelivery(render::RenderBackend::Vulkan, false, false); }
TEST(SceneDeliveryRunner, VulkanThreadedFlights) { RunSceneDelivery(render::RenderBackend::Vulkan, true, false); }
TEST(SceneDeliveryRunner, D3D12ExitDrainsPublishedFlights) { RunSceneDelivery(render::RenderBackend::D3D12, true, true); }
TEST(SceneDeliveryRunner, VulkanExitDrainsPublishedFlights) { RunSceneDelivery(render::RenderBackend::Vulkan, true, true); }
#endif

}  // namespace
}  // namespace radray
