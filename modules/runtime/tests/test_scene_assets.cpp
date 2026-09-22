#include "scene_test_support.h"
#include <gtest/gtest.h>

#include <coroutine>
#include <semaphore>
#include <thread>

#include <radray/runtime/application.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>

namespace radray {
namespace {

struct Lifetime {
    std::thread::id GameThread{std::this_thread::get_id()};
    uint32_t Unloaded{0};
    uint32_t Destroyed{0};
};

class TrackedMesh final : public StaticMesh {
public:
    TrackedMesh(MeshResource cpu, GpuMesh gpu, shared_ptr<Lifetime> life)
        : StaticMesh(std::move(cpu), {{0, 0, 3, 0, 2}}, Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), std::move(gpu)),
          _life(std::move(life)) {}
    ~TrackedMesh() noexcept override {
        EXPECT_EQ(std::this_thread::get_id(), _life->GameThread);
        EXPECT_EQ(_life->Unloaded, 1u);
        ++_life->Destroyed;
    }
    void OnUnload(AssetManager&) override {
        EXPECT_EQ(std::this_thread::get_id(), _life->GameThread);
        ++_life->Unloaded;
    }

private:
    shared_ptr<Lifetime> _life;
};

class TrackedAsset final : public Asset {
public:
    explicit TrackedAsset(shared_ptr<Lifetime> life) : _life(std::move(life)) {}
    ~TrackedAsset() noexcept override {
        EXPECT_EQ(std::this_thread::get_id(), _life->GameThread);
        EXPECT_EQ(_life->Unloaded, 1u);
        ++_life->Destroyed;
    }
    void OnUnload(AssetManager&) override {
        EXPECT_EQ(std::this_thread::get_id(), _life->GameThread);
        ++_life->Unloaded;
    }

private:
    shared_ptr<Lifetime> _life;
};

AssetId MeshId(uint32_t value) { return AssetId{value, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}; }

unique_ptr<StaticMesh> MakeMesh(shared_ptr<Lifetime> life, uint32_t tag) {
    const array<float, 9> vertices{0, 0, 0, 1, 0, 0, 0, 1, 0};
    const array<uint32_t, 3> indices{0, 1, 2};
    MeshResource cpu;
    cpu.Bins.emplace_back(std::as_bytes(std::span{vertices}));
    cpu.Bins.emplace_back(std::as_bytes(std::span{indices}));
    MeshPrimitive primitive;
    primitive.VertexCount = 3;
    primitive.VertexBuffers.push_back({"POSITION", 0, 0, VertexDataType::FLOAT, 3, 0, 12});
    primitive.IndexBuffer = {1, 3, 0, 4};
    cpu.Primitives.push_back(std::move(primitive));
    GpuMesh gpu;
    gpu.Draws.emplace_back();
    gpu.Draws.back().Ibv.Offset = tag;
    return make_unique<TrackedMesh>(std::move(cpu), std::move(gpu), std::move(life));
}

class Gate {
public:
    Gate() = default;
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;
    ~Gate() { Resume(); }
    void Resume() {
        const auto handle = std::exchange(_handle, {});
        if (handle) handle.resume();
    }
    struct Awaiter {
        Gate* Owner;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) const noexcept { Owner->_handle = handle; }
        void await_resume() const noexcept {}
    };
    Awaiter Wait() { return {this}; }

private:
    std::coroutine_handle<> _handle{};
};

task<AssetLoadResult> LoadMesh(Gate* gate, shared_ptr<Lifetime> life, uint32_t tag) {
    co_await gate->Wait();
    const auto stop = co_await CurrentStopToken();
    if (stop.stop_requested()) co_await StopCurrentTask();
    co_return AssetLoadResult::Success(MakeMesh(std::move(life), tag));
}

class SceneAssets : public testing::Test {
protected:
    StaticMeshComponent* Add(StreamingAssetRef<StaticMesh> mesh) {
        auto* component = Owner->AddComponent<StaticMeshComponent>();
        component->SetStaticMesh(std::move(mesh));
        return component;
    }
    StreamingAssetRef<StaticMesh> Ready(uint32_t id, shared_ptr<Lifetime> life) {
        return Assets.AddReady<StaticMesh>(MeshId(id), MakeMesh(std::move(life), id));
    }
    StreamingAssetRef<StaticMesh> Loading(uint32_t id, Gate* gate, shared_ptr<Lifetime> life) {
        return Assets.Load({.Id = MeshId(id), .Task = LoadMesh(gate, std::move(life), id)}).CastTo<StaticMesh>();
    }
    void Prepare(uint32_t flight) { test::PrepareScene(GameWorld, Render, flight); }
    void Complete(uint32_t flight) { test::CompleteFrame(Render, flight, true); }
    void Flush() {
        test::CollectScene(GameWorld, Render, Batch);
        Data.Apply(Batch);
        Batch.Clear();
    }

    Application App;
    AssetManager Assets;
    RenderAssetLifetime AssetLifetime{3};
    RenderSystem Render{&App, 3};
    test::ScopedWorld GameWorld;
    SceneId RenderId{test::ConnectWorld(GameWorld, Render)};
    Actor* Owner{GameWorld.SpawnActor()};
    SceneUpdateBatch Batch;
    RenderScene Data;
};

TEST_F(SceneAssets, StationaryObjectsStayAliveUntilTheRemovalFlightCompletes) {
    for (uint32_t count : {1u, 2u, 3u}) {
        auto life = make_shared<Lifetime>();
        auto* component = Add(Ready(count, life));
        const auto id = component->GetShapeId();
        for (uint32_t i = 0; i < count; ++i) {
            Prepare(i);
            if (i != 0) EXPECT_TRUE(test::SceneBatch(Render, RenderId, i).Empty());
            test::ConsumeFrame(Render, i);
            ASSERT_TRUE(Render.GetSceneRT(RenderId)->GetStaticMesh(id)->Mesh.GetRenderMesh());
            EXPECT_EQ(Render.GetSceneRT(RenderId)->GetStaticMesh(id)->Mesh.GetRenderMesh()->Draws[0].Ibv.Offset, count);
        }
        Owner->RemoveComponent(component);
        for (uint32_t i = 0; i < count; ++i) {
            Assets.Pump();
            EXPECT_EQ(life->Destroyed, 0u);
            Complete(i);
        }
        Assets.Pump();
        EXPECT_EQ(life->Destroyed, 0u);
        // The last binding remains owned until its removal is delivered and completed.
        Prepare(0);
        test::ConsumeFrame(Render, 0);
        EXPECT_FALSE(Render.GetSceneRT(RenderId)->GetStaticMesh(id));
        Complete(0);
        Assets.Pump();
        EXPECT_EQ(life->Destroyed, 1u);
    }
}

TEST_F(SceneAssets, RenderThreadCanReadOldGeometryWhileGameThreadRebindsAndDeletes) {
    auto oldLife = make_shared<Lifetime>();
    auto newLife = make_shared<Lifetime>();
    auto* component = Add(Ready(1, oldLife));
    const auto id = component->GetShapeId();
    Prepare(0);
    Render.PublishFrameGT(0);
    std::binary_semaphore applied{0};
    std::binary_semaphore changed{0};
    std::thread rt([&]() {
        Render.ConsumeRenderUpdates(0, 1);
        applied.release();
        changed.acquire();
        const auto view = Render.GetSceneRT(RenderId)->GetStaticMesh(id);
        EXPECT_EQ(view->Mesh.GetRenderMesh()->Draws[0].Ibv.Offset, 1u);
        Render.ConsumeRenderUpdates(1, 2);
        EXPECT_EQ(Render.GetSceneRT(RenderId)->GetStaticMesh(id)->Mesh.GetRenderMesh()->Draws[0].Ibv.Offset, 2u);
        Render.ConsumeRenderUpdates(2, 3);
        EXPECT_FALSE(Render.GetSceneRT(RenderId)->GetStaticMesh(id));
    });
    applied.acquire();
    component->SetStaticMesh(Ready(2, newLife));
    Prepare(1);
    Render.PublishFrameGT(1);
    Owner->RemoveComponent(component);
    Prepare(2);
    Render.PublishFrameGT(2);
    Assets.Pump();
    EXPECT_EQ(oldLife->Destroyed, 0u);
    EXPECT_EQ(newLife->Destroyed, 0u);
    changed.release();
    rt.join();
    Complete(0);
    Assets.Pump();
    EXPECT_EQ(oldLife->Destroyed, 0u);
    EXPECT_EQ(newLife->Destroyed, 0u);
    Complete(1);
    Assets.Pump();
    EXPECT_EQ(oldLife->Destroyed, 1u);
    EXPECT_EQ(newLife->Destroyed, 0u);
    Complete(2);
    Assets.Pump();
    EXPECT_EQ(newLife->Destroyed, 1u);
}

TEST_F(SceneAssets, SharedMeshIsRetainedOnceAndRemovedOnlyAfterItsLastUser) {
    auto life = make_shared<Lifetime>();
    auto mesh = Ready(1, life);
    vector<StaticMeshComponent*> components;
    for (int i = 0; i < 1000; ++i) components.push_back(Add(mesh));
    mesh.Reset();
    Flush();
    for (auto* component : components) component->MarkRenderStateDirty();
    Flush();
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 0u);
    while (components.size() > 1) {
        Owner->RemoveComponent(components.back());
        components.pop_back();
    }
    Flush();
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 0u);
    Owner->RemoveComponent(components.back());
    Flush();
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 1u);
}

TEST_F(SceneAssets, ReplacementRetiresOldAssetOnItsOwnCompletion) {
    auto oldLife = make_shared<Lifetime>();
    auto* component = Add(Ready(1, oldLife));
    const auto id = component->GetShapeId();
    Prepare(0);
    test::ConsumeFrame(Render, 0);
    Complete(0);
    component->SetStaticMesh(Ready(2, make_shared<Lifetime>()));
    Assets.Pump();
    EXPECT_EQ(oldLife->Destroyed, 0u);
    Prepare(0);
    test::ConsumeFrame(Render, 0);
    EXPECT_EQ(Render.GetSceneRT(RenderId)->GetStaticMesh(id)->Mesh.GetRenderMesh()->Draws[0].Ibv.Offset, 2u);
    Complete(0);
    Assets.Pump();
    EXPECT_EQ(oldLife->Destroyed, 1u);
}

TEST_F(SceneAssets, RemovingOneWaiterDoesNotCancelTheSharedLoad) {
    Gate gate;
    auto life = make_shared<Lifetime>();
    auto loading = Loading(1, &gate, life);
    auto* removed = Add(loading);
    auto* remaining = Add(loading);
    const auto id = remaining->GetShapeId();
    Flush();
    EXPECT_FALSE(Data.GetStaticMesh(id)->Mesh.GetRenderMesh());
    Owner->RemoveComponent(removed);
    Flush();
    gate.Resume();
    Assets.Pump();
    EXPECT_TRUE(loading.IsReady());
    remaining->SetRelativeLocation({7, 8, 9});
    test::CollectScene(GameWorld, Render, Batch);
    ASSERT_EQ(Batch.MeshStates.size(), 1u);
    EXPECT_EQ(Batch.MeshStates[0].Id, id);
    EXPECT_FLOAT_EQ(Batch.MeshStates[0].LocalToWorld(0, 3), 7);
    EXPECT_TRUE(Batch.Transforms.empty());
    EXPECT_TRUE(Batch.MeshStates[0].Mesh.GetRenderMesh());
}

TEST_F(SceneAssets, OldRequestCompletionCannotDirtyOrReplaceTheNewBinding) {
    Gate gate;
    auto loading = Loading(1, &gate, make_shared<Lifetime>());
    auto* component = Add(loading);
    const auto id = component->GetShapeId();
    Flush();
    component->SetStaticMesh(Ready(2, make_shared<Lifetime>()));
    Flush();
    gate.Resume();
    Assets.Pump();
    ASSERT_TRUE(loading.IsReady());
    test::CollectScene(GameWorld, Render, Batch);
    EXPECT_TRUE(Batch.Empty());
    EXPECT_EQ(Data.GetStaticMesh(id)->Mesh.GetRenderMesh()->Draws[0].Ibv.Offset, 2u);
}

TEST_F(SceneAssets, ReusedPrimitiveSlotNotifiesOnlyItsCurrentRegistration) {
    Gate gate;
    auto loading = Loading(1, &gate, make_shared<Lifetime>());
    auto* component = Add(loading);
    const auto oldId = component->GetShapeId();
    Flush();
    Owner->RemoveComponent(component);
    component = Add(loading);
    const auto id = component->GetShapeId();
    EXPECT_NE(id, oldId);
    Flush();
    gate.Resume();
    Assets.Pump();
    test::CollectScene(GameWorld, Render, Batch);
    ASSERT_EQ(Batch.MeshStates.size(), 1u);
    EXPECT_EQ(Batch.MeshStates[0].Id, id);
}

TEST_F(SceneAssets, ReturningToTheSameLoadingRequestArmsANewWaiter) {
    Gate gate;
    auto loading = Loading(1, &gate, make_shared<Lifetime>());
    auto* component = Add(loading);
    component->SetStaticMesh({});
    component->SetStaticMesh(loading);
    Flush();
    gate.Resume();
    Assets.Pump();
    test::CollectScene(GameWorld, Render, Batch);
    ASSERT_EQ(Batch.MeshStates.size(), 1u);
    EXPECT_TRUE(Batch.MeshStates[0].Mesh.GetRenderMesh());
}

TEST_F(SceneAssets, CancelingSharedLoadLeavesNoGeometryAndReleasesWaiters) {
    Gate gate;
    auto life = make_shared<Lifetime>();
    auto loading = Loading(1, &gate, life);
    auto* component = Add(loading);
    const auto id = component->GetShapeId();
    Flush();
    loading.Cancel();
    gate.Resume();
    Assets.Pump();
    EXPECT_TRUE(loading.IsCanceled());
    test::CollectScene(GameWorld, Render, Batch);
    EXPECT_TRUE(Batch.Empty());
    EXPECT_FALSE(Data.GetStaticMesh(id)->Mesh.GetRenderMesh());
    Owner->RemoveComponent(component);
    GameWorld.FinalizeWorldGT();
    loading.Reset();
    Assets.Pump();
    EXPECT_EQ(Assets.GetAssetCount(), 0u);
}

TEST_F(SceneAssets, FailedLoadLeavesNoGeometryAndReleasesWaiters) {
    Gate gate;
    auto loading = Assets.Load({.Id = MeshId(1), .Task = [](Gate* pending) -> task<AssetLoadResult> {
                                    co_await pending->Wait();
                                    co_return AssetLoadResult::Failure();
                                }(&gate)})
                       .CastTo<StaticMesh>();
    auto* component = Add(loading);
    const auto id = component->GetShapeId();
    Flush();
    gate.Resume();
    Assets.Pump();
    EXPECT_TRUE(loading.IsFaulted());
    test::CollectScene(GameWorld, Render, Batch);
    EXPECT_TRUE(Batch.Empty());
    EXPECT_FALSE(Data.GetStaticMesh(id)->Mesh.GetRenderMesh());
    Owner->RemoveComponent(component);
    GameWorld.FinalizeWorldGT();
    loading.Reset();
    Assets.Pump();
    EXPECT_EQ(Assets.GetAssetCount(), 0u);
}

TEST_F(SceneAssets, DestroyingWorldCancelsWaitersWithoutCancelingSharedLoading) {
    Gate gate;
    auto life = make_shared<Lifetime>();
    auto loading = Loading(1, &gate, life);
    {
        test::ScopedWorld temporary;
        temporary.SpawnActor()->AddComponent<StaticMeshComponent>()->SetStaticMesh(loading);
    }
    gate.Resume();
    Assets.Pump();
    EXPECT_TRUE(loading.IsReady());
    loading.Reset();
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 1u);
}

TEST_F(SceneAssets, ShutdownReleasesUnpublishedPinsAfterPublishedCompletion) {
    auto life = make_shared<Lifetime>();
    auto* component = Add(Ready(1, life));
    Prepare(0);
    test::ConsumeFrame(Render, 0);
    Owner->RemoveComponent(component);
    Prepare(1);
    Complete(0);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 0u);
    Render.BeginStoppingGT();
    Render.AbandonUnpublishedFrameGT(1);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 1u);
    test::DisconnectWorld(GameWorld);
    Render.OnShutdown();
    test::DisconnectWorld(GameWorld);
    Render.OnShutdown();
}

TEST_F(SceneAssets, EmptyAssetIdCanStillBeAReadyBinding) {
    auto life = make_shared<Lifetime>();
    auto* component = Add(Assets.AddReady<StaticMesh>({}, MakeMesh(life, 1)));
    Prepare(0);
    Owner->RemoveComponent(component);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 0u);
    test::ConsumeFrame(Render, 0);
    Complete(0);
    Prepare(1);
    test::ConsumeFrame(Render, 1);
    Complete(1);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 1u);
}

TEST_F(SceneAssets, SameFrameRemovalAndNewBindingKeepTheSharedAssetResident) {
    auto life = make_shared<Lifetime>();
    auto asset = Ready(1, life);
    auto* old = Add(asset);
    Prepare(0);
    test::ConsumeFrame(Render, 0);
    Complete(0);
    Owner->RemoveComponent(old);
    auto* replacement = Add(asset);
    asset.Reset();
    Prepare(0);
    test::ConsumeFrame(Render, 0);
    Complete(0);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 0u);
    EXPECT_TRUE(Render.GetSceneRT(RenderId)->GetStaticMesh(replacement->GetShapeId())->Mesh.GetRenderMesh());
    Owner->RemoveComponent(replacement);
    Prepare(1);
    test::ConsumeFrame(Render, 1);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 0u);
    test::CompleteFrame(Render, 1, false);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 1u);
}

TEST_F(SceneAssets, WorldDestructionLeavesPublishedViewsOwnedUntilQuiescentShutdown) {
    auto life = make_shared<Lifetime>();
    auto world = make_unique<test::ScopedWorld>();
    const auto otherScene = test::ConnectWorld(*world, Render);
    auto* component = world->SpawnActor()->AddComponent<StaticMeshComponent>();
    component->SetStaticMesh(Ready(1, life));
    const auto id = component->GetShapeId();
    test::PrepareScene(*world, Render, 0);
    world.reset();
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 0u);
    test::ConsumeFrame(Render, 0);
    EXPECT_EQ(Render.GetSceneRT(otherScene)->GetStaticMesh(id)->Mesh.GetRenderMesh()->Draws[0].Ibv.Offset, 1u);
    Complete(0);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 0u);
    test::DisconnectWorld(GameWorld);
    Render.OnShutdown();
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 1u);
}

TEST_F(SceneAssets, LifetimeAcceptsDifferentAssetTypesAndRetiresOnlyTheLastUse) {
    auto meshLife = make_shared<Lifetime>();
    auto otherLife = make_shared<Lifetime>();
    auto mesh = Ready(1, meshLife);
    auto other = Assets.AddReady<TrackedAsset>(MeshId(2), make_unique<TrackedAsset>(otherLife));
    for (int i = 0; i < 1000; ++i) AssetLifetime.AddUse(mesh.AsAny());
    AssetLifetime.AddUse(other.AsAny());
    mesh.Reset();
    other.Reset();
    for (int i = 0; i < 999; ++i) AssetLifetime.RemoveUse(MeshId(1));
    AssetLifetime.RemoveUse(MeshId(2));
    AssetLifetime.SealRetirements(0);
    Assets.Pump();
    EXPECT_EQ(meshLife->Destroyed, 0u);
    EXPECT_EQ(otherLife->Destroyed, 0u);
    AssetLifetime.ReleaseFlight(0);
    Assets.Pump();
    EXPECT_EQ(meshLife->Destroyed, 0u);
    EXPECT_EQ(otherLife->Destroyed, 1u);
    AssetLifetime.RemoveUse(MeshId(1));
    AssetLifetime.SealRetirements(1);
    AssetLifetime.ReleaseFlight(1);
    Assets.Pump();
    EXPECT_EQ(meshLife->Destroyed, 1u);
}

TEST_F(SceneAssets, RepeatedZeroUseTransitionsMergeBeforeSealAndCanRetireAgainLater) {
    auto life = make_shared<Lifetime>();
    auto asset = Ready(1, life);
    AssetLifetime.AddUse(asset.AsAny());
    for (int i = 0; i < 1000; ++i) {
        AssetLifetime.RemoveUse(MeshId(1));
        AssetLifetime.AddUse(asset.AsAny());
    }
    asset.Reset();
    AssetLifetime.SealRetirements(0);
    AssetLifetime.ReleaseFlight(0);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 0u);
    AssetLifetime.RemoveUse(MeshId(1));
    AssetLifetime.SealRetirements(1);
    AssetLifetime.ReleaseFlight(1);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 1u);
}

TEST_F(SceneAssets, RebindingAfterSealSurvivesTheOldRetirementCompletion) {
    auto life = make_shared<Lifetime>();
    auto asset = Ready(1, life);
    AssetLifetime.AddUse(asset.AsAny());
    AssetLifetime.RemoveUse(MeshId(1));
    AssetLifetime.SealRetirements(0);
    AssetLifetime.AddUse(asset.AsAny());
    asset.Reset();
    AssetLifetime.SealRetirements(1);
    AssetLifetime.ReleaseFlight(0);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 0u);
    AssetLifetime.ReleaseFlight(1);
    AssetLifetime.RemoveUse(MeshId(1));
    AssetLifetime.SealRetirements(2);
    AssetLifetime.ReleaseFlight(2);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 1u);
}

TEST_F(SceneAssets, DestroyingOneSceneRetainsSharedAssetUntilLastSceneCompletes) {
    auto life = make_shared<Lifetime>();
    auto asset = Ready(31, life);
    auto* firstMesh = Add(asset);
    const auto firstPrimitive = firstMesh->GetShapeId();
    test::ScopedWorld other;
    const auto otherScene = test::ConnectWorld(other, Render);
    auto* secondMesh = other.SpawnActor()->AddComponent<StaticMeshComponent>();
    secondMesh->SetStaticMesh(asset);
    const auto secondPrimitive = secondMesh->GetShapeId();
    asset.Reset();
    GameWorld.CollectRenderUpdates();
    other.CollectRenderUpdates();
    Render.SealFrameGT(0);
    // Destroy the source before RT has applied its creation.
    GameWorld.DestroyActor(Owner);
    Owner = nullptr;
    test::DisconnectWorld(GameWorld);
    Render.SealFrameGT(1);
    test::ConsumeFrame(Render, 0);
    EXPECT_TRUE(Render.GetSceneRT(RenderId)->GetStaticMesh(firstPrimitive)->Mesh.GetRenderMesh());
    test::ConsumeFrame(Render, 1);
    EXPECT_FALSE(Render.GetSceneRT(RenderId));
    EXPECT_TRUE(Render.GetSceneRT(otherScene)->GetStaticMesh(secondPrimitive)->Mesh.GetRenderMesh());
    Complete(0);
    Complete(1);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 0u);
    other.DestroyActor(other.GetActors().front().get());
    test::DisconnectWorld(other);
    Render.SealFrameGT(2);
    test::ConsumeFrame(Render, 2);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 0u);
    Complete(2);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 1u);
}

TEST_F(SceneAssets, ReconnectRejectsOldReadyRequestAndPublishesWhilePaused) {
    Gate oldGate;
    Gate newGate;
    auto oldLife = make_shared<Lifetime>();
    auto newLife = make_shared<Lifetime>();
    auto oldRequest = Loading(41, &oldGate, oldLife);
    auto newRequest = Loading(42, &newGate, newLife);
    auto* component = Add(oldRequest);
    Prepare(0);
    test::ConsumeFrame(Render, 0);
    Complete(0);
    const auto oldScene = RenderId;
    const auto oldPrimitive = component->GetShapeId();
    test::DisconnectWorld(GameWorld);
    component->SetStaticMesh(newRequest);
    GameWorld.SetTickEnabled(false);
    RenderId = test::ConnectWorld(GameWorld, Render);
    EXPECT_NE(RenderId, oldScene);
    EXPECT_EQ(component->GetShapeId(), oldPrimitive);
    Prepare(0);
    test::ConsumeFrame(Render, 0);
    Complete(0);
    oldGate.Resume();
    Assets.Pump();
    Prepare(0);
    EXPECT_TRUE(test::SceneBatch(Render, RenderId, 0).Empty());
    test::ConsumeFrame(Render, 0);
    Complete(0);
    component->SetRelativeLocation({71, 0, 0});
    newGate.Resume();
    Assets.Pump();
    Prepare(0);
    test::ConsumeFrame(Render, 0);
    auto view = Render.GetSceneRT(RenderId)->GetStaticMesh(component->GetShapeId());
    ASSERT_TRUE(view);
    ASSERT_TRUE(view->Mesh.GetRenderMesh());
    EXPECT_EQ(view->Mesh.GetRenderMesh()->Draws[0].Ibv.Offset, 42u);
    EXPECT_FLOAT_EQ(view->LocalToWorld(0, 3), 71);
    Complete(0);
}

TEST_F(SceneAssets, StandaloneWriterCoalescesReplacementAndRetainsFinalBinding) {
    auto firstLife = make_shared<Lifetime>();
    auto secondLife = make_shared<Lifetime>();
    auto first = Ready(51, firstLife);
    auto second = Ready(52, secondLife);
    const auto scene = Render.CreateSceneGT();
    auto writer = Render.GetSceneWriterGT(scene);
    const auto primitive = writer->CreateShape();
    writer->SetStaticMesh(primitive, first, Eigen::Matrix4f::Identity());
    writer->SetStaticMesh(primitive, second, Eigen::Matrix4f::Identity());
    first.Reset();
    second.Reset();
    Render.SealFrameGT(0);
    EXPECT_EQ(test::SceneBatch(Render, scene, 0).MeshStates.size(), 1u);
    test::ConsumeFrame(Render, 0);
    EXPECT_EQ(Render.GetSceneRT(scene)->GetStaticMesh(primitive)->Mesh.GetRenderMesh()->Draws[0].Ibv.Offset, 52u);
    Complete(0);
    Assets.Pump();
    EXPECT_EQ(firstLife->Destroyed, 1u);
    EXPECT_EQ(secondLife->Destroyed, 0u);
    Render.DestroySceneGT(scene);
    Render.SealFrameGT(1);
    test::ConsumeFrame(Render, 1);
    Assets.Pump();
    EXPECT_EQ(secondLife->Destroyed, 0u);
    Complete(1);
    Assets.Pump();
    EXPECT_EQ(secondLife->Destroyed, 1u);
}

TEST(SceneAssetsDeathTest, RejectsConflictingAssetIdentity) {
    AssetManager source;
    AssetManager otherManager;
    RenderAssetLifetime lifetime{2};
    test::ScopedWorld world;
    auto asset = source.AddReady<StaticMesh>(MeshId(1), MakeMesh(make_shared<Lifetime>(), 1));
    world.SpawnActor()->AddComponent<StaticMeshComponent>()->SetStaticMesh(asset);
    world.CollectRenderUpdates();
    EXPECT_FALSE(world.GetRenderSceneId());
    lifetime.AddUse(asset.AsAny());
    auto other = otherManager.AddReady<StaticMesh>(MeshId(1), MakeMesh(make_shared<Lifetime>(), 2));
    EXPECT_DEATH(lifetime.AddUse(other.AsAny()), "");
    EXPECT_DEATH(lifetime.AddUse({}), "");
    EXPECT_DEATH(lifetime.RemoveUse(MeshId(2)), "");
    EXPECT_DEATH(lifetime.SealRetirements(2), "");
    EXPECT_DEATH(lifetime.ReleaseFlight(2), "");
    lifetime.RemoveUse(MeshId(1));
    EXPECT_DEATH(lifetime.RemoveUse(MeshId(1)), "");
    lifetime.SealRetirements(0);
    EXPECT_DEATH(lifetime.SealRetirements(0), "");
}

}  // namespace
}  // namespace radray
