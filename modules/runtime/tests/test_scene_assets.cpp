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
    void Prepare(uint32_t flight) { Render.PrepareFrameGT(GameWorld, {.FlightIndex = flight}); }
    void Complete(uint32_t flight) { Render.OnFlightCompletedGT({.FlightIndex = flight, .GpuWorkCompleted = true}); }
    void Flush() {
        GameWorld.FlushRenderUpdates(Batch);
        Data.Apply(Batch);
        Batch.Clear();
    }

    Application App;
    AssetManager Assets;
    World GameWorld;
    RenderSystem Render{&App, 3};
    Actor* Owner{GameWorld.SpawnActor()};
    SceneUpdateBatch Batch;
    Scene Data;

    void SetUp() override { Render.SetAssetManager(&Assets); }
};

TEST_F(SceneAssets, StationaryObjectsArePinnedInEveryFlightUntilItsCompletion) {
    for (uint32_t count : {1u, 2u, 3u}) {
        auto life = make_shared<Lifetime>();
        auto* component = Add(Ready(count, life));
        const auto id = component->GetPrimitiveId();
        for (uint32_t i = 0; i < count; ++i) {
            Prepare(i);
            if (i != 0) EXPECT_TRUE(Render.GetFrameUpdateBatchGT(i).Empty());
            Render.ConsumeRenderUpdates(i);
            ASSERT_TRUE(Render.GetScene().GetStaticMesh(id)->Mesh.RenderMesh);
            EXPECT_EQ(Render.GetScene().GetStaticMesh(id)->Mesh.RenderMesh->Draws[0].Ibv.Offset, count);
        }
        Owner->RemoveComponent(component);
        for (uint32_t i = 0; i < count; ++i) {
            Assets.Pump();
            EXPECT_EQ(life->Destroyed, 0u);
            Complete(i);
        }
        Assets.Pump();
        EXPECT_EQ(life->Destroyed, 1u);
        // Removal must not dereference the now-retired asset in the persistent Scene.
        Prepare(0);
        Render.ConsumeRenderUpdates(0);
        EXPECT_FALSE(Render.GetScene().GetStaticMesh(id));
        Complete(0);
    }
}

TEST_F(SceneAssets, RenderThreadCanReadOldGeometryWhileGameThreadRebindsAndDeletes) {
    auto oldLife = make_shared<Lifetime>();
    auto newLife = make_shared<Lifetime>();
    auto* component = Add(Ready(1, oldLife));
    const auto id = component->GetPrimitiveId();
    Prepare(0);
    std::binary_semaphore applied{0};
    std::binary_semaphore changed{0};
    std::thread rt([&]() {
        Render.ConsumeRenderUpdates(0);
        applied.release();
        changed.acquire();
        const auto view = Render.GetScene().GetStaticMesh(id);
        EXPECT_EQ(view->Mesh.RenderMesh->Draws[0].Ibv.Offset, 1u);
        Render.ConsumeRenderUpdates(1);
        EXPECT_EQ(Render.GetScene().GetStaticMesh(id)->Mesh.RenderMesh->Draws[0].Ibv.Offset, 2u);
        Render.ConsumeRenderUpdates(2);
        EXPECT_FALSE(Render.GetScene().GetStaticMesh(id));
    });
    applied.acquire();
    component->SetStaticMesh(Ready(2, newLife));
    Prepare(1);
    Owner->RemoveComponent(component);
    Prepare(2);
    Assets.Pump();
    EXPECT_EQ(oldLife->Destroyed, 0u);
    EXPECT_EQ(newLife->Destroyed, 0u);
    changed.release();
    rt.join();
    Complete(2);
    Assets.Pump();
    EXPECT_EQ(oldLife->Destroyed, 0u);
    EXPECT_EQ(newLife->Destroyed, 0u);
    Complete(0);
    Assets.Pump();
    EXPECT_EQ(oldLife->Destroyed, 1u);
    EXPECT_EQ(newLife->Destroyed, 0u);
    Complete(1);
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
    vector<StreamingAssetRef<StaticMesh>> refs;
    GameWorld.RetainRenderAssets(&Assets, refs);
    ASSERT_EQ(refs.size(), 1u);
    refs.clear();
    while (components.size() > 1) {
        Owner->RemoveComponent(components.back());
        components.pop_back();
    }
    Flush();
    GameWorld.RetainRenderAssets(&Assets, refs);
    ASSERT_EQ(refs.size(), 1u);
    refs.clear();
    Owner->RemoveComponent(components.back());
    Flush();
    GameWorld.RetainRenderAssets(&Assets, refs);
    EXPECT_TRUE(refs.empty());
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 1u);
}

TEST_F(SceneAssets, ReplaceDoesNotReadTheRetiredAsset) {
    auto oldLife = make_shared<Lifetime>();
    auto* component = Add(Ready(1, oldLife));
    const auto id = component->GetPrimitiveId();
    Prepare(0);
    Render.ConsumeRenderUpdates(0);
    Complete(0);
    component->SetStaticMesh(Ready(2, make_shared<Lifetime>()));
    Assets.Pump();
    EXPECT_EQ(oldLife->Destroyed, 1u);
    Prepare(0);
    Render.ConsumeRenderUpdates(0);
    EXPECT_EQ(Render.GetScene().GetStaticMesh(id)->Mesh.RenderMesh->Draws[0].Ibv.Offset, 2u);
    Complete(0);
}

TEST_F(SceneAssets, RemovingOneWaiterDoesNotCancelTheSharedLoad) {
    Gate gate;
    auto life = make_shared<Lifetime>();
    auto loading = Loading(1, &gate, life);
    auto* removed = Add(loading);
    auto* remaining = Add(loading);
    const auto id = remaining->GetPrimitiveId();
    Flush();
    EXPECT_FALSE(Data.GetStaticMesh(id)->Mesh.RenderMesh);
    Owner->RemoveComponent(removed);
    Flush();
    gate.Resume();
    Assets.Pump();
    EXPECT_TRUE(loading.IsReady());
    remaining->SetRelativeLocation({7, 8, 9});
    GameWorld.FlushRenderUpdates(Batch);
    ASSERT_EQ(Batch.MeshStates.size(), 1u);
    EXPECT_EQ(Batch.MeshStates[0].Id, id);
    EXPECT_FLOAT_EQ(Batch.MeshStates[0].LocalToWorld(0, 3), 7);
    EXPECT_TRUE(Batch.Transforms.empty());
    EXPECT_TRUE(Batch.MeshStates[0].Mesh.RenderMesh);
}

TEST_F(SceneAssets, OldRequestCompletionCannotDirtyOrReplaceTheNewBinding) {
    Gate gate;
    auto loading = Loading(1, &gate, make_shared<Lifetime>());
    auto* component = Add(loading);
    const auto id = component->GetPrimitiveId();
    Flush();
    component->SetStaticMesh(Ready(2, make_shared<Lifetime>()));
    Flush();
    gate.Resume();
    Assets.Pump();
    ASSERT_TRUE(loading.IsReady());
    GameWorld.FlushRenderUpdates(Batch);
    EXPECT_TRUE(Batch.Empty());
    EXPECT_EQ(Data.GetStaticMesh(id)->Mesh.RenderMesh->Draws[0].Ibv.Offset, 2u);
}

TEST_F(SceneAssets, ReusedPrimitiveSlotNotifiesOnlyItsCurrentRegistration) {
    Gate gate;
    auto loading = Loading(1, &gate, make_shared<Lifetime>());
    auto* component = Add(loading);
    const auto oldId = component->GetPrimitiveId();
    Flush();
    Owner->RemoveComponent(component);
    component = Add(loading);
    const auto id = component->GetPrimitiveId();
    EXPECT_NE(id, oldId);
    Flush();
    gate.Resume();
    Assets.Pump();
    GameWorld.FlushRenderUpdates(Batch);
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
    GameWorld.FlushRenderUpdates(Batch);
    ASSERT_EQ(Batch.MeshStates.size(), 1u);
    EXPECT_TRUE(Batch.MeshStates[0].Mesh.RenderMesh);
}

TEST_F(SceneAssets, CancelingSharedLoadLeavesNoGeometryAndReleasesWaiters) {
    Gate gate;
    auto life = make_shared<Lifetime>();
    auto loading = Loading(1, &gate, life);
    auto* component = Add(loading);
    const auto id = component->GetPrimitiveId();
    Flush();
    loading.Cancel();
    gate.Resume();
    Assets.Pump();
    EXPECT_TRUE(loading.IsCanceled());
    GameWorld.FlushRenderUpdates(Batch);
    EXPECT_TRUE(Batch.Empty());
    EXPECT_FALSE(Data.GetStaticMesh(id)->Mesh.RenderMesh);
    Owner->RemoveComponent(component);
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
    const auto id = component->GetPrimitiveId();
    Flush();
    gate.Resume();
    Assets.Pump();
    EXPECT_TRUE(loading.IsFaulted());
    GameWorld.FlushRenderUpdates(Batch);
    EXPECT_TRUE(Batch.Empty());
    EXPECT_FALSE(Data.GetStaticMesh(id)->Mesh.RenderMesh);
    Owner->RemoveComponent(component);
    loading.Reset();
    Assets.Pump();
    EXPECT_EQ(Assets.GetAssetCount(), 0u);
}

TEST_F(SceneAssets, DestroyingWorldCancelsWaitersWithoutCancelingSharedLoading) {
    Gate gate;
    auto life = make_shared<Lifetime>();
    auto loading = Loading(1, &gate, life);
    {
        World temporary;
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
    Render.ConsumeRenderUpdates(0);
    Prepare(1);
    Owner->RemoveComponent(component);
    Complete(0);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 0u);
    Render.AbandonUnpublishedFrameGT(1);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 1u);
    Render.OnShutdown();
    Render.OnShutdown();
}

TEST_F(SceneAssets, EmptyAssetIdCanStillBeAReadyBinding) {
    auto life = make_shared<Lifetime>();
    auto* component = Add(Assets.AddReady<StaticMesh>({}, MakeMesh(life, 1)));
    Prepare(0);
    Owner->RemoveComponent(component);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 0u);
    Render.ConsumeRenderUpdates(0);
    Complete(0);
    Assets.Pump();
    EXPECT_EQ(life->Destroyed, 1u);
}

TEST(SceneAssetsDeathTest, MissingOrDifferentReadyObjectCannotPinAPublishedView) {
    AssetManager source;
    AssetManager wrong;
    World world;
    auto life = make_shared<Lifetime>();
    world.SpawnActor()->AddComponent<StaticMeshComponent>()->SetStaticMesh(source.AddReady<StaticMesh>(MeshId(1), MakeMesh(life, 1)));
    SceneUpdateBatch batch;
    world.FlushRenderUpdates(batch);
    vector<StreamingAssetRef<StaticMesh>> refs;
    EXPECT_DEATH(world.RetainRenderAssets(nullptr, refs), "");
    EXPECT_DEATH(world.RetainRenderAssets(&wrong, refs), "");
    auto other = wrong.AddReady<StaticMesh>(MeshId(1), MakeMesh(make_shared<Lifetime>(), 2));
    EXPECT_DEATH(world.RetainRenderAssets(&wrong, refs), "");
}

}  // namespace
}  // namespace radray
