#include "scene_test_support.h"
#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include "gpu_runtime_test_support.h"

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
    for (uint32_t count : {1u, 2u, 3u, 8u}) {
        SCOPED_TRACE(count);
        Application app;
        RenderSystem render{&app, count};
        test::ScopedWorld world{&app};
        const auto sceneId = test::ConnectWorld(world, render);
        Nullable<const RenderScene*> scene{nullptr};
        for (uint32_t round = 0; round < 8; ++round) {
            for (uint32_t flight = 0; flight < count; ++flight) {
                test::PrepareScene(world, render, flight);
            }
            for (uint32_t flight = 0; flight < count; ++flight) {
                test::ConsumeFrame(render, flight);
                if (!scene) scene = render.GetSceneRT(sceneId);
                EXPECT_EQ(render.GetSceneRT(sceneId), scene);
                test::CompleteFrame(render, flight, false);
            }
        }
    }
}

TEST(SceneDelivery, AbandonDoesNotPublishOrRequireCompletion) {
    for (bool abandonAll : {false, true}) {
        Application app;
        RenderSystem render{&app, 3};
        test::ScopedWorld world{&app};
        const auto sceneId = test::ConnectWorld(world, render);
        const ShapeId id = world.SpawnActor()->AddComponent<PrimitiveComponent>()->GetShapeId();
        test::PrepareScene(world, render, 2);
        ASSERT_EQ(test::SceneBatch(render, sceneId, 2).CreateShapes, vector<ShapeId>{id});
        render.BeginStoppingGT();
        if (abandonAll)
            render.AbandonUnpublishedFramesGT();
        else
            render.AbandonUnpublishedFrameGT(2);
        EXPECT_TRUE(render.GetFrameUpdatesRT(2).empty());
        EXPECT_FALSE(render.GetSceneRT(sceneId));
        test::DisconnectWorld(world);
        render.OnShutdown();
        test::DisconnectWorld(world);
        render.OnShutdown();
    }
}

TEST(SceneDelivery, PrimitiveUpdatesAcrossFlightsWithoutDrawing) {
    for (uint32_t count : {1u, 2u, 3u, 8u}) {
        SCOPED_TRACE(count);
        Application app;
        RenderSystem render{&app, count};
        test::ScopedWorld world{&app};
        const auto sceneId = test::ConnectWorld(world, render);
        auto* actor = world.SpawnActor();
        Nullable<PrimitiveComponent*> component{nullptr};
        vector<ShapeId> expected(count);
        for (uint32_t round = 0; round < 4; ++round) {
            for (uint32_t flight = 0; flight < count; ++flight) {
                if (component) actor->RemoveComponent(component.Get());
                component = actor->AddComponent<PrimitiveComponent>();
                expected[flight] = component->GetShapeId();
                test::PrepareScene(world, render, flight);
            }
            for (uint32_t flight = 0; flight < count; ++flight) {
                test::ConsumeFrame(render, flight);
                EXPECT_TRUE(render.GetSceneRT(sceneId)->ContainsShape(expected[flight]));
                if (flight > 0) EXPECT_FALSE(render.GetSceneRT(sceneId)->ContainsShape(expected[flight - 1]));
                test::CompleteFrame(render, flight, false);
            }
        }
    }
}

TEST(SceneDeliveryDeathTest, RejectsInvalidFlightIndices) {
    Application app;
    test::ScopedWorld world{&app};
    RenderSystem render{&app, 1};
    EXPECT_DEATH(render.GetFrameUpdatesRT(1), "");
    EXPECT_DEATH(test::PrepareScene(world, render, 1), "");
    EXPECT_DEATH(test::ConsumeFrame(render, 1), "");
    EXPECT_DEATH(test::CompleteFrame(render, 1), "");
    EXPECT_DEATH(render.AbandonUnpublishedFrameGT(1), "");
}

TEST(SceneDeliveryDeathTest, DuplicateAndOutOfOrderPacketsAreRejectedBeforeApply) {
    Application app;
    RenderSystem renderer{&app, 2};
    const auto id = renderer.CreateSceneGT();
    auto* writer = renderer.GetSceneWriterGT(id).Get();
    const auto primitive = writer->CreateShape();
    writer->SetStaticMesh(primitive, {}, Eigen::Matrix4f::Identity());
    renderer.SealFrameGT(0);
    test::ConsumeFrame(renderer, 0);
    EXPECT_DEATH(renderer.ConsumeRenderUpdates(0, 1), "");
    test::CompleteFrame(renderer, 0);
    auto transform = Eigen::Matrix4f::Identity().eval();
    transform(0, 3) = 1;
    writer->SetTransform(primitive, transform);
    renderer.SealFrameGT(0);
    renderer.PublishFrameGT(0);
    transform(0, 3) = 2;
    writer->SetTransform(primitive, transform);
    renderer.SealFrameGT(1);
    renderer.PublishFrameGT(1);
    EXPECT_DEATH(renderer.ConsumeRenderUpdates(1, 3), "");
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(id)->GetStaticMesh(primitive)->LocalToWorld(0, 3), 0);
    renderer.ConsumeRenderUpdates(0, 2);
    renderer.ConsumeRenderUpdates(1, 3);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(id)->GetStaticMesh(primitive)->LocalToWorld(0, 3), 2);
    EXPECT_DEATH(renderer.OnFlightCompletedGT({.FlightIndex = 0, .FrameSerial = 1}), "");
    test::CompleteFrame(renderer, 0);
    test::CompleteFrame(renderer, 1);
    EXPECT_DEATH(renderer.AbandonUnpublishedFramesGT(), "");
    renderer.BeginStoppingGT();
    renderer.AbandonUnpublishedFramesGT();
    EXPECT_DEATH(renderer.SealFrameGT(0), "");
}

TEST(SceneDelivery, ApplyWaitsForThePreviousCpuReader) {
    RenderScene scene;
    SceneUpdateBatch create;
    const ShapeId id{0, 0};
    create.CreateShapes.push_back(id);
    scene.Apply(create);
    std::binary_semaphore borrowed{0}, releaseReader{0}, applied{0};
    std::thread reader{[&, lease = scene.AcquireRead()] {
        borrowed.release();
        releaseReader.acquire();
        EXPECT_TRUE(scene.ContainsShape(id));
    }};
    borrowed.acquire();
    SceneUpdateBatch remove;
    remove.RemoveShapes.push_back(id);
    std::thread apply{[&] { scene.Apply(remove); applied.release(); }};
    EXPECT_FALSE(applied.try_acquire_for(std::chrono::milliseconds{30}));
    releaseReader.release();
    reader.join();
    apply.join();
    EXPECT_TRUE(applied.try_acquire());
    EXPECT_FALSE(scene.ContainsShape(id));
}

class CpuSceneDeliveryApp final : public Application {
public:
    explicit CpuSceneDeliveryApp(uint32_t flightCount) : _flightCount(flightCount) {}

protected:
    void OnInit() override {
        EXPECT_FALSE(GetWindowManager());
        EXPECT_FALSE(GetGpuSystem());
        EXPECT_FALSE(GetAssetManager());
        ASSERT_TRUE(GetRenderSystem());
        ASSERT_TRUE(GetWorldManager());
        _firstWorld = GetWorldManager()->CreateWorld();
        _secondWorld = GetWorldManager()->CreateWorld();
        GetWorldManager()->RequestRenderConnection(_firstWorld, true);
        GetWorldManager()->RequestRenderConnection(_secondWorld, true);
        _firstActor = GetWorldManager()->GetWorld(_firstWorld)->SpawnActor();
        _firstComponent = _firstActor->AddComponent<PrimitiveComponent>();
        GetWorldManager()->GetWorld(_secondWorld)->SpawnActor()->AddComponent<PrimitiveComponent>();
    }

    void OnUpdate(const AppUpdateContext& ctx) override {
        EXPECT_EQ(ctx.FlightIndex, _updates % _flightCount);
        EXPECT_EQ(ctx.LastFrameLatency.count(), 0.0f);
        ++_updates;
        if (_updates == 1) {
            _firstScene = *GetWorldManager()->GetWorld(_firstWorld)->GetRenderSceneId();
            _secondScene = *GetWorldManager()->GetWorld(_secondWorld)->GetRenderSceneId();
            _firstShape = _firstComponent->GetShapeId();
        } else if (_updates == 2) {
            ASSERT_TRUE(GetRenderSystem()->GetSceneRT(_firstScene));
            EXPECT_TRUE(GetRenderSystem()->GetSceneRT(_firstScene)->ContainsShape(_firstShape));
            EXPECT_TRUE(GetRenderSystem()->GetSceneRT(_secondScene));
            GetWorldManager()->DestroyWorld(_secondWorld);
        } else if (_updates == 3) {
            EXPECT_FALSE(GetRenderSystem()->GetSceneRT(_secondScene));
            EXPECT_TRUE(GetRenderSystem()->GetSceneRT(_firstScene)->ContainsShape(_firstShape));
        } else if (_updates == 4) {
            RequestExit();
        }
    }

    void OnRender(AppFrameContext&) override { ADD_FAILURE() << "CPU frame loop must not record GPU work"; }
    void OnRenderFrameComplete(const FlightCompletion&) override { ADD_FAILURE() << "CPU frame loop has no GPU completion"; }
    void OnShutdown() override {
        EXPECT_EQ(_updates, 4u);
        EXPECT_TRUE(GetRenderSystem()->GetSceneRT(_firstScene)->ContainsShape(_firstShape));
        EXPECT_FALSE(GetRenderSystem()->GetSceneRT(_secondScene));
    }

private:
    uint32_t _flightCount;
    uint32_t _updates{0};
    WorldId _firstWorld;
    WorldId _secondWorld;
    SceneId _firstScene;
    SceneId _secondScene;
    ShapeId _firstShape;
    Nullable<Actor*> _firstActor{nullptr};
    Nullable<PrimitiveComponent*> _firstComponent{nullptr};
};

TEST(SceneDeliveryRunner, CpuOnlyWorldSceneFrames) {
    for (uint32_t flightCount : {1u, 2u, 3u, 8u}) {
        SCOPED_TRACE(flightCount);
        CpuSceneDeliveryApp app{flightCount};
        RuntimeStartupResult startup;
        EXPECT_EQ(app.Run({.Backend = render::RenderBackend::D3D12,
                           .FlightDataCount = flightCount,
                           .Systems = ApplicationSystem::Render | ApplicationSystem::World}, startup), 0);
        EXPECT_EQ(startup.Status, RuntimeStartupStatus::Started) << startup.Reason;
    }
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
        GetWorldManager()->RequestRenderConnection(_worldId, true);
        _actor = GetWorldManager()->GetWorld(_worldId)->SpawnActor();
        _component = _actor->AddComponent<PrimitiveComponent>();
    }

    void OnUpdate(const AppUpdateContext&) override {
        ++_updates;
        if (_updates == 1) {
            _sceneId = *GetWorldManager()->GetWorld(_worldId)->GetRenderSceneId();
            _initialId = _component->GetShapeId();
            _latestId = _initialId;
        }
        const auto count = GetGpuSystem()->GetFlightDataCount();
        const bool exit = _drainOnExit ? _updates == count : _updates == 13;
        if (!exit) {
            if (_updates == 2) {
                _actor->RemoveComponent(_component.Get());
                _component = _actor->AddComponent<PrimitiveComponent>();
                _latestId = _component->GetShapeId();
            }
            return;
        }
        if (_drainOnExit && count > 1) {
            // Runner closes window operations after setting its exit flag, before joining RT.
            _tasks.Spawn(ReleaseRenderOnCancellation());
        }
        if (_drainOnExit) {
            auto* native = GetWindowManager()->GetMainWindow()->GetNativeWindow();
            ::SendMessageW(static_cast<HWND>(native->GetNativeHandler()), WM_CLOSE, 0, 0);
        } else {
            RequestExit();
        }
    }

    void OnRender(AppFrameContext& ctx) override {
        ++Recorded;
        if (ctx.FrameSerial() == 1) EXPECT_TRUE(GetRenderSystem()->GetSceneRT(_sceneId)->ContainsShape(_initialId));
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
        EXPECT_EQ(scene && scene->ContainsShape(_latestId), PublishedFrames > 0);
        if (scene && _latestId != _initialId) EXPECT_FALSE(scene->ContainsShape(_initialId));
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
    ShapeId _initialId;
    ShapeId _latestId;
    uint64_t _updates{0};
    std::binary_semaphore _renderGate{0};
    TaskScope _tasks;
};

void RunSceneDelivery(render::RenderBackend backend, bool threaded, bool drainOnExit) {
    for (uint32_t count : {1u, 2u, 3u, 8u}) {
        SCOPED_TRACE(count);
        test::RuntimeLogCapture logs;
        SceneDeliveryApp app{drainOnExit};
        const ApplicationSystems systems = drainOnExit
            ? ApplicationSystem::Window | ApplicationSystem::Gpu | ApplicationSystem::Render | ApplicationSystem::World
            : ApplicationSystem::Gpu | ApplicationSystem::Render | ApplicationSystem::World;
        auto run = test::RunApplication(app, {.Backend = backend, .EnableValidation = true, .Multithreaded = threaded, .EnableSynchronizationValidation = true, .WindowTitle = "RenderScene delivery", .WindowWidth = 80, .WindowHeight = 60, .FlightDataCount = count, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO, .Systems = systems, .EnableGpuFrameProfiler = false});
        if (test::CanSkipRuntimeStartup(backend, run.Startup)) GTEST_SKIP() << run.Startup.Reason;
        ASSERT_EQ(run.Startup.Status, RuntimeStartupStatus::Started) << run.Startup.Reason;
        ASSERT_EQ(run.ExitCode, 0);
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
        GetWorldManager()->RequestRenderConnection(_firstWorld, true);
        _mesh = GetWorldManager()->GetWorld(_firstWorld)->SpawnActor()->AddComponent<StaticMeshComponent>();
        GetWorldManager()->GetWorld(_firstWorld)->SetTickEnabled(false);
        _secondWorld = GetWorldManager()->CreateWorld();
        GetWorldManager()->RequestRenderConnection(_secondWorld, true);
        GetWorldManager()->GetWorld(_secondWorld)->SpawnActor()->AddComponent<StaticMeshComponent>();
    }

    void OnUpdate(const AppUpdateContext&) override {
        ++_updates;
        if (_updates == 1) {
            _firstScene = *GetWorldManager()->GetWorld(_firstWorld)->GetRenderSceneId();
            _secondScene = *GetWorldManager()->GetWorld(_secondWorld)->GetRenderSceneId();
        }
        _mesh->SetRelativeLocation({static_cast<float>(_updates), 0, 0});
        if (_updates == 2) {
            GetWorldManager()->DestroyWorld(_secondWorld);
            EXPECT_FALSE(GetWorldManager()->GetWorld(_secondWorld));
        }
        if (_updates == 3) {
            const auto replacement = GetWorldManager()->CreateWorld();
            _replacementWorld = replacement;
            EXPECT_EQ(replacement.Index, _secondWorld.Index);
            EXPECT_GT(replacement.Generation, _secondWorld.Generation);
            GetWorldManager()->RequestRenderConnection(replacement, true);
            GetWorldManager()->GetWorld(replacement)->SpawnActor()->AddComponent<StaticMeshComponent>();
        }
        if (_updates == 5) {
            GetWorldManager()->RequestReconnect(_firstWorld);
        }
        if (_updates == 9) {
            RequestExit();
        }
    }

    void OnRender(AppFrameContext& ctx) override {
        const auto frame = ctx.FrameSerial();
        for (const auto& update : GetRenderSystem()->GetFrameUpdatesRT(ctx.FlightIndex())) {
            if (update.Create && frame == 3) _replacementScene = update.Id;
            if (update.Create && frame == 5) _reattachedScene = update.Id;
        }
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
        EXPECT_TRUE(GetRenderSystem()->GetSceneRT(*GetWorldManager()->GetWorld(_replacementWorld)->GetRenderSceneId()));
        const auto scene = GetRenderSystem()->GetSceneRT(*GetWorldManager()->GetWorld(_firstWorld)->GetRenderSceneId());
        ASSERT_TRUE(scene);
        EXPECT_FLOAT_EQ(scene->GetStaticMesh(scene->GetStaticMeshes()[0])->LocalToWorld(0, 3), 8);
    }

private:
    WorldId _firstWorld;
    WorldId _secondWorld;
    WorldId _replacementWorld;
    SceneId _firstScene;
    SceneId _secondScene;
    SceneId _replacementScene;
    SceneId _reattachedScene;
    Nullable<StaticMeshComponent*> _mesh{nullptr};
    uint32_t _updates{0};
};

void RunMultiWorldDelivery(render::RenderBackend backend, bool threaded) {
    for (uint32_t count : {1u, 2u, 3u, 8u}) {
        SCOPED_TRACE(count);
        test::RuntimeLogCapture logs;
        MultiWorldDeliveryApp app;
        auto run = test::RunApplication(app, {.Backend = backend, .EnableValidation = true, .Multithreaded = threaded, .EnableSynchronizationValidation = true, .FlightDataCount = count, .Systems = ApplicationSystem::Gpu | ApplicationSystem::Render | ApplicationSystem::World, .EnableGpuFrameProfiler = false});
        if (test::CanSkipRuntimeStartup(backend, run.Startup)) GTEST_SKIP() << run.Startup.Reason;
        ASSERT_EQ(run.Startup.Status, RuntimeStartupStatus::Started) << run.Startup.Reason;
        ASSERT_EQ(run.ExitCode, 0);
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
