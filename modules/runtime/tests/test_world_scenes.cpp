#include "scene_test_support.h"

#include <gtest/gtest.h>
#include <functional>

#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/world_manager.h>

namespace radray {
namespace {

class CountingActor final : public Actor {
public:
    CountingActor(uint32_t& ticks, uint32_t& destroyed) : _ticks(ticks), _destroyed(destroyed) {}
    ~CountingActor() noexcept override { ++_destroyed; }
    void Tick(float) override {
        ++_ticks;
        if (Action) Action();
    }
    void OnDestroyed() override {
        if (DestroyAction) DestroyAction();
    }
    std::function<void()> Action;
    std::function<void()> DestroyAction;

private:
    uint32_t& _ticks;
    uint32_t& _destroyed;
};

TEST(WorldManager, ExplicitOwnershipPauseAndDeferredDestruction) {
    uint32_t ticksA = 0, ticksB = 0, destroyedA = 0, destroyedB = 0;
    WorldManager manager;
    EXPECT_FALSE(manager.GetWorld({}));
    const auto a = manager.CreateWorld();
    const auto b = manager.CreateWorld();
    manager.GetWorld(a)->SpawnActor<CountingActor>(ticksA, destroyedA);
    manager.GetWorld(b)->SpawnActor<CountingActor>(ticksB, destroyedB);
    manager.GetWorld(a)->SetTickEnabled(false);
    manager.Tick(0);
    EXPECT_EQ(ticksA, 0u);
    EXPECT_EQ(ticksB, 1u);
    manager.GetWorld(a)->SetTickEnabled(true);
    manager.DestroyWorld(b);
    EXPECT_FALSE(manager.GetWorld(b));
    EXPECT_EQ(destroyedB, 0u);
    manager.Tick(0);
    EXPECT_EQ(destroyedB, 1u);
    EXPECT_EQ(ticksA, 1u);
    EXPECT_EQ(ticksB, 1u);
    const auto replacement = manager.CreateWorld();
    EXPECT_EQ(replacement.Index, b.Index);
    EXPECT_GT(replacement.Generation, b.Generation);
    EXPECT_FALSE(manager.GetWorld(b));
}

TEST(WorldManager, TickSnapshotSurvivesCreationAndSelfDestruction) {
    uint32_t ticksA = 0, ticksB = 0, destroyedA = 0, destroyedB = 0;
    WorldManager manager;
    const auto a = manager.CreateWorld();
    WorldId b;
    auto actor = manager.GetWorld(a)->SpawnActor<CountingActor>(ticksA, destroyedA);
    actor->Action = [&]() {
        b = manager.CreateWorld();
        manager.GetWorld(b)->SpawnActor<CountingActor>(ticksB, destroyedB);
        manager.DestroyWorld(a);
        EXPECT_EQ(destroyedA, 0u);
    };
    manager.Tick(0);
    EXPECT_EQ(ticksA, 1u);
    EXPECT_EQ(ticksB, 0u);
    EXPECT_EQ(destroyedA, 1u);
    EXPECT_FALSE(manager.GetWorld(a));
    manager.Tick(0);
    EXPECT_EQ(ticksB, 1u);
}

TEST(WorldManager, DestroyedWorldIsSkippedBeforeItsScheduledTick) {
    uint32_t ticksA = 0, ticksB = 0, destroyedA = 0, destroyedB = 0;
    WorldManager manager;
    const auto a = manager.CreateWorld();
    const auto b = manager.CreateWorld();
    auto* actorA = manager.GetWorld(a)->SpawnActor<CountingActor>(ticksA, destroyedA);
    auto* actorB = manager.GetWorld(b)->SpawnActor<CountingActor>(ticksB, destroyedB);
    EXPECT_FALSE(manager.GetWorld(a)->GetApplication());
    actorA->Action = [&]() { manager.DestroyWorld(b); };
    actorB->DestroyAction = [&]() {
        EXPECT_FALSE(manager.GetWorld(b));
        EXPECT_TRUE(manager.GetWorld(a));
    };
    manager.Tick(0);
    EXPECT_EQ(ticksA, 1u);
    EXPECT_EQ(ticksB, 0u);
    EXPECT_EQ(destroyedB, 1u);
}

TEST(WorldManager, ClearHidesAllWorldsDuringCallbacksAndInvalidatesIds) {
    uint32_t ticks = 0, destroyed = 0;
    WorldManager manager;
    const WorldManager& readOnly = manager;
    const auto a = manager.CreateWorld();
    const auto b = manager.CreateWorld();
    for (const auto id : {a, b}) {
        manager.GetWorld(id)->SpawnActor<CountingActor>(ticks, destroyed)->DestroyAction = [&]() {
            EXPECT_FALSE(readOnly.GetWorld(a));
            EXPECT_FALSE(readOnly.GetWorld(b));
        };
    }
    EXPECT_TRUE(readOnly.GetWorld(a));
    manager.Clear();
    EXPECT_EQ(destroyed, 2u);
    manager.Clear();
    for (const auto old : {a, b}) {
        const auto replacement = manager.CreateWorld();
        EXPECT_EQ(replacement.Index, old.Index);
        EXPECT_GT(replacement.Generation, old.Generation);
        EXPECT_FALSE(readOnly.GetWorld(old));
    }
}

TEST(WorldManager, CreatedDuringTickCollectsImmediatelyAndClearRetiresScenes) {
    uint32_t ticks = 0, destroyed = 0;
    Application app;
    RenderSystem renderer{&app, 2};
    WorldManager manager{&app, &renderer};
    const auto parent = manager.CreateWorld();
    EXPECT_EQ(manager.GetWorld(parent)->GetApplication().Get(), &app);
    auto* actor = manager.GetWorld(parent)->SpawnActor<CountingActor>(ticks, destroyed);
    WorldId child;
    SceneId scene;
    PrimitiveId primitive;
    actor->Action = [&]() {
        child = manager.CreateWorld();
        scene = manager.AttachWorldToRendering(child);
        auto world = manager.GetWorld(child);
        world->SetTickEnabled(false);
        auto* mesh = world->SpawnActor()->AddComponent<StaticMeshComponent>();
        mesh->SetRelativeLocation({12, 0, 0});
        primitive = mesh->GetPrimitiveId();
    };
    manager.Tick(0);
    manager.CollectRenderUpdates();
    renderer.SealFrameGT(0);
    manager.Clear();
    EXPECT_FALSE(manager.GetWorld(child));
    EXPECT_EQ(destroyed, 1u);
    renderer.SealFrameGT(1);
    renderer.ConsumeRenderUpdates(0);
    ASSERT_TRUE(renderer.GetSceneRT(scene));
    ASSERT_TRUE(renderer.GetSceneRT(scene)->GetStaticMesh(primitive));
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(scene)->GetStaticMesh(primitive)->LocalToWorld(0, 3), 12);
    renderer.ConsumeRenderUpdates(1);
    EXPECT_FALSE(renderer.GetSceneRT(scene));
    const auto beforeCompletion = renderer.CreateSceneGT();
    EXPECT_NE(beforeCompletion.Index, scene.Index);
    renderer.OnFlightCompletedGT({.FlightIndex = 0});
    renderer.OnFlightCompletedGT({.FlightIndex = 1});
    const auto afterCompletion = renderer.CreateSceneGT();
    EXPECT_EQ(afterCompletion.Index, scene.Index);
    EXPECT_GT(afterCompletion.Generation, scene.Generation);
}

TEST(WorldManagerDeathTest, RejectsReentrantTickCollectionAndClear) {
    uint32_t ticks = 0, destroyed = 0;
    WorldManager manager;
    const auto id = manager.CreateWorld();
    auto* actor = manager.GetWorld(id)->SpawnActor<CountingActor>(ticks, destroyed);
    actor->Action = [&]() { manager.Tick(0); };
    EXPECT_DEATH(manager.Tick(0), "");
    actor->Action = [&]() { manager.CollectRenderUpdates(); };
    EXPECT_DEATH(manager.Tick(0), "");
    actor->Action = [&]() { manager.Clear(); };
    EXPECT_DEATH(manager.Tick(0), "");
    actor->DestroyAction = [&]() { manager.CreateWorld(); };
    EXPECT_DEATH(manager.Clear(), "");
    actor->DestroyAction = {};
}

class ManagerCollectionProbe final : public SceneComponent {
public:
    explicit ManagerCollectionProbe(std::function<void()> action) : _action(std::move(action)) {}

protected:
    void CreateRenderState(SceneWriter&) override { MarkRenderStateDirty(); }
    void CollectRenderUpdates(SceneWriter&, RenderDirtyFlags) override { _action(); }

private:
    std::function<void()> _action;
};

TEST(WorldManagerDeathTest, RejectsCollectionMutationAndMissingRenderService) {
    WorldManager cpuOnly;
    const auto cpuWorld = cpuOnly.CreateWorld();
    EXPECT_DEATH(cpuOnly.AttachWorldToRendering(cpuWorld), "");
    Application app;
    RenderSystem renderer{&app, 1};
    WorldManager manager{&app, &renderer};
    const auto id = manager.CreateWorld();
    manager.AttachWorldToRendering(id);
    auto* actor = manager.GetWorld(id)->SpawnActor();
    auto* probe = actor->AddComponent<ManagerCollectionProbe>([&]() { manager.DestroyWorld(id); });
    EXPECT_DEATH(manager.CollectRenderUpdates(), "");
    actor->RemoveComponent(probe);
    actor->AddComponent<ManagerCollectionProbe>([&]() { manager.DetachWorldFromRendering(id); });
    EXPECT_DEATH(manager.CollectRenderUpdates(), "");
}

class RegistrationProbe final : public SceneComponent {
public:
    RegistrationProbe(uint32_t& registered, uint32_t& unregistered) : _registered(registered), _unregistered(unregistered) {}
    void OnRegister() override { ++_registered; }
    void OnUnregister() override { ++_unregistered; }

private:
    uint32_t& _registered;
    uint32_t& _unregistered;
};

TEST(WorldScenes, AttachExistingPausedWorldAndReconnectWithoutGameCallbacks) {
    uint32_t registrations = 0, unregistrations = 0;
    Application app;
    RenderSystem renderer{&app, 3};
    World world;
    auto* actor = world.SpawnActor();
    actor->AddComponent<RegistrationProbe>(registrations, unregistrations);
    auto* mesh = actor->AddComponent<StaticMeshComponent>();
    mesh->SetRelativeLocation({7, 0, 0});
    EXPECT_FALSE(mesh->GetPrimitiveId().IsValid());
    world.SetTickEnabled(false);
    world.CollectRenderUpdates();
    const auto first = world.AttachToRendering(renderer);
    const auto oldPrimitive = mesh->GetPrimitiveId();
    test::PrepareScene(world, renderer, 0);
    world.DetachFromRendering();
    EXPECT_FALSE(world.GetRenderSceneId());
    EXPECT_FALSE(mesh->GetPrimitiveId().IsValid());
    mesh->SetRelativeLocation({19, 0, 0});
    const auto second = world.AttachToRendering(renderer);
    EXPECT_NE(first, second);
    const auto newPrimitive = mesh->GetPrimitiveId();
    test::PrepareScene(world, renderer, 1);
    renderer.ConsumeRenderUpdates(0);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(first)->GetStaticMesh(oldPrimitive)->LocalToWorld(0, 3), 7);
    renderer.ConsumeRenderUpdates(1);
    EXPECT_FALSE(renderer.GetSceneRT(first));
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(second)->GetStaticMesh(newPrimitive)->LocalToWorld(0, 3), 19);
    EXPECT_EQ(registrations, 1u);
    EXPECT_EQ(unregistrations, 0u);
    renderer.OnFlightCompletedGT({.FlightIndex = 0});
    renderer.OnFlightCompletedGT({.FlightIndex = 1});
    world.DestroyActor(actor);
    EXPECT_EQ(unregistrations, 1u);
}

TEST(WorldScenes, StandaloneWritersIsolateIdentitiesAndCoalesceUpdates) {
    Application app;
    RenderSystem renderer{&app, 2};
    const auto a = renderer.CreateSceneGT();
    const auto b = renderer.CreateSceneGT();
    auto wa = renderer.GetSceneWriterGT(a);
    auto wb = renderer.GetSceneWriterGT(b);
    const auto pa = wa->CreatePrimitive();
    const auto pb = wb->CreatePrimitive();
    EXPECT_EQ(pa, pb);
    wa->SetStaticMesh(pa, {}, Eigen::Matrix4f::Identity());
    wb->SetStaticMesh(pb, {}, Eigen::Matrix4f::Identity());
    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    for (int i = 0; i < 20; ++i) {
        transform(0, 3) = static_cast<float>(i);
        wa->SetTransform(pa, transform);
    }
    renderer.SealFrameGT(0);
    const auto& batch = test::SceneBatch(renderer, a, 0);
    EXPECT_EQ(batch.CreatePrimitives.size(), 1u);
    EXPECT_EQ(batch.MeshStates.size(), 1u);
    EXPECT_TRUE(batch.Transforms.empty());
    renderer.ConsumeRenderUpdates(0);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(a)->GetStaticMesh(pa)->LocalToWorld(0, 3), 19);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(b)->GetStaticMesh(pb)->LocalToWorld(0, 3), 0);
    renderer.OnFlightCompletedGT({.FlightIndex = 0});
    renderer.DestroySceneGT(a);
    EXPECT_FALSE(renderer.GetSceneWriterGT(a));
    transform(0, 3) = 37;
    wb->SetTransform(pb, transform);
    renderer.SealFrameGT(1);
    renderer.ConsumeRenderUpdates(1);
    EXPECT_FALSE(renderer.GetSceneRT(a));
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(b)->GetStaticMesh(pb)->LocalToWorld(0, 3), 37);
    const auto beforeCompletion = renderer.CreateSceneGT();
    EXPECT_NE(beforeCompletion.Index, a.Index);
    renderer.OnFlightCompletedGT({.FlightIndex = 1});
    const auto afterCompletion = renderer.CreateSceneGT();
    EXPECT_EQ(afterCompletion.Index, a.Index);
    EXPECT_GT(afterCompletion.Generation, a.Generation);
    EXPECT_FALSE(renderer.GetSceneRT(a));
    EXPECT_FALSE(renderer.GetSceneWriterGT(a));
}

TEST(WorldScenes, SameFrameCreateAndDestroyAndRetirementAcrossFlights) {
    for (uint32_t count : {1u, 2u, 3u}) {
        Application app;
        RenderSystem renderer{&app, count};
        const auto stable = renderer.CreateSceneGT();
        for (uint32_t round = 0; round < 3; ++round) {
            vector<SceneId> removed;
            for (uint32_t flight = 0; flight < count; ++flight) {
                const auto scene = renderer.CreateSceneGT();
                renderer.GetSceneWriterGT(scene)->CreatePrimitive();
                renderer.DestroySceneGT(scene);
                removed.push_back(scene);
                renderer.SealFrameGT(flight);
            }
            for (uint32_t flight = 0; flight < count; ++flight) {
                renderer.ConsumeRenderUpdates(flight);
                EXPECT_FALSE(renderer.GetSceneRT(removed[flight]));
                EXPECT_TRUE(renderer.GetSceneRT(stable));
                renderer.OnFlightCompletedGT({.FlightIndex = flight});
                EXPECT_FALSE(renderer.GetSceneWriterGT(removed[flight]));
            }
        }
    }
}

TEST(WorldScenesDeathTest, ClaimedAndClosingScenesRejectExternalWrites) {
    Application app;
    RenderSystem renderer{&app, 1};
    World world;
    const auto attached = world.AttachToRendering(renderer);
    EXPECT_FALSE(renderer.GetSceneWriterGT(attached));
    EXPECT_DEATH(renderer.DestroySceneGT(attached), "");
    EXPECT_DEATH(renderer.OnShutdown(), "");
    EXPECT_DEATH(world.AttachToRendering(renderer), "");
    const auto standalone = renderer.CreateSceneGT();
    auto writer = renderer.GetSceneWriterGT(standalone);
    const auto primitive = writer->CreatePrimitive();
    EXPECT_DEATH(writer->SetTransform(primitive, Eigen::Matrix4f::Identity()), "");
    renderer.DestroySceneGT(standalone);
    EXPECT_DEATH(writer->RemovePrimitive(primitive), "");
    EXPECT_DEATH(writer->CreatePrimitive(), "");
}

}  // namespace
}  // namespace radray
