#include <radray/runtime/components/render_component.h>
#include "scene_test_support.h"

#include <gtest/gtest.h>
#include <functional>

#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/world_manager.h>

namespace radray {
namespace {

class WorldCountingActor final : public Actor {
public:
    WorldCountingActor(uint32_t& ticks, uint32_t& destroyed) : _ticks(ticks), _destroyed(destroyed) { SetTickEnabled(true); }
    ~WorldCountingActor() noexcept override { ++_destroyed; }
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
    test::ScopedWorldManager manager;
    EXPECT_FALSE(manager.GetWorld({}));
    const auto a = manager.CreateWorld();
    const auto b = manager.CreateWorld();
    manager.GetWorld(a)->SpawnActor<WorldCountingActor>(ticksA, destroyedA);
    manager.GetWorld(b)->SpawnActor<WorldCountingActor>(ticksB, destroyedB);
    manager.GetWorld(a)->SetTickEnabled(false);
    manager.Tick(0);
    manager.FinalizeWorldsGT();
    EXPECT_EQ(ticksA, 0u);
    EXPECT_EQ(ticksB, 1u);
    manager.GetWorld(a)->SetTickEnabled(true);
    manager.DestroyWorld(b);
    EXPECT_FALSE(manager.GetWorld(b));
    EXPECT_EQ(destroyedB, 0u);
    manager.Tick(0);
    manager.FinalizeWorldsGT();
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
    test::ScopedWorldManager manager;
    const auto a = manager.CreateWorld();
    WorldId b;
    auto actor = manager.GetWorld(a)->SpawnActor<WorldCountingActor>(ticksA, destroyedA);
    actor->Action = [&]() {
        b = manager.CreateWorld();
        manager.GetWorld(b)->SpawnActor<WorldCountingActor>(ticksB, destroyedB);
        manager.DestroyWorld(a);
        EXPECT_EQ(destroyedA, 0u);
    };
    manager.Tick(0);
    manager.FinalizeWorldsGT();
    EXPECT_EQ(ticksA, 1u);
    EXPECT_EQ(ticksB, 0u);
    EXPECT_EQ(destroyedA, 1u);
    EXPECT_FALSE(manager.GetWorld(a));
    manager.Tick(0);
    manager.FinalizeWorldsGT();
    EXPECT_EQ(ticksB, 1u);
}

TEST(WorldManager, DestroyedWorldIsSkippedBeforeItsScheduledTick) {
    uint32_t ticksA = 0, ticksB = 0, destroyedA = 0, destroyedB = 0;
    test::ScopedWorldManager manager;
    const auto a = manager.CreateWorld();
    const auto b = manager.CreateWorld();
    auto* actorA = manager.GetWorld(a)->SpawnActor<WorldCountingActor>(ticksA, destroyedA);
    auto* actorB = manager.GetWorld(b)->SpawnActor<WorldCountingActor>(ticksB, destroyedB);
    EXPECT_FALSE(manager.GetWorld(a)->GetApplication());
    actorA->Action = [&]() { manager.DestroyWorld(b); };
    actorB->DestroyAction = [&]() {
        EXPECT_FALSE(manager.GetWorld(b));
        EXPECT_TRUE(manager.GetWorld(a));
    };
    manager.Tick(0);
    manager.FinalizeWorldsGT();
    EXPECT_EQ(ticksA, 1u);
    EXPECT_EQ(ticksB, 0u);
    EXPECT_EQ(destroyedB, 1u);
}

TEST(WorldManager, TickIterationSurvivesGrowthAndDefersNewWorlds) {
    uint32_t firstTicks = 0, lastTicks = 0, newTicks = 0, destroyed = 0;
    test::ScopedWorldManager manager;
    const auto first = manager.CreateWorld();
    const auto last = manager.CreateWorld();
    auto* actor = manager.GetWorld(first)->SpawnActor<WorldCountingActor>(firstTicks, destroyed);
    manager.GetWorld(last)->SpawnActor<WorldCountingActor>(lastTicks, destroyed);
    actor->Action = [&] {
        if (firstTicks != 1) return;
        for (uint32_t i = 0; i < 128; ++i) {
            const auto added = manager.CreateWorld();
            manager.GetWorld(added)->SpawnActor<WorldCountingActor>(newTicks, destroyed);
        }
    };
    manager.Tick(0);
    EXPECT_EQ(firstTicks, 1u);
    EXPECT_EQ(lastTicks, 1u);
    EXPECT_EQ(newTicks, 0u);
    manager.Tick(0);
    EXPECT_EQ(firstTicks, 2u);
    EXPECT_EQ(lastTicks, 2u);
    EXPECT_EQ(newTicks, 128u);
}

TEST(WorldManager, DestructionCallbacksQueueTheNextBatchAfterIdentityReuse) {
    uint32_t ticks = 0, firstDestroyed = 0, secondDestroyed = 0, addedDestroyed = 0;
    test::ScopedWorldManager manager;
    const auto first = manager.CreateWorld();
    const auto second = manager.CreateWorld();
    auto* actor = manager.GetWorld(first)->SpawnActor<WorldCountingActor>(ticks, firstDestroyed);
    manager.GetWorld(second)->SpawnActor<WorldCountingActor>(ticks, secondDestroyed);
    WorldId added;
    actor->DestroyAction = [&] {
        added = manager.CreateWorld();
        EXPECT_EQ(added.Index, first.Index);
        EXPECT_GT(added.Generation, first.Generation);
        manager.GetWorld(added)->SpawnActor<WorldCountingActor>(ticks, addedDestroyed);
        EXPECT_EQ(manager.DestroyWorld(second), LifecycleRequestResult::Accepted);
        EXPECT_EQ(manager.DestroyWorld(added), LifecycleRequestResult::Accepted);
    };
    manager.DestroyWorld(first);
    manager.FinalizeWorldsGT();
    EXPECT_EQ(firstDestroyed, 1u);
    EXPECT_EQ(secondDestroyed, 0u);
    EXPECT_EQ(addedDestroyed, 0u);
    EXPECT_FALSE(manager.GetWorld(first));
    EXPECT_FALSE(manager.GetWorld(second));
    EXPECT_FALSE(manager.GetWorld(added));
    manager.FinalizeWorldsGT();
    EXPECT_EQ(secondDestroyed, 1u);
    EXPECT_EQ(addedDestroyed, 1u);
}

TEST(WorldManager, ClearHidesAllWorldsDuringCallbacksAndInvalidatesIds) {
    uint32_t ticks = 0, destroyed = 0;
    test::ScopedWorldManager manager;
    const WorldManager& readOnly = manager;
    const auto a = manager.CreateWorld();
    const auto b = manager.CreateWorld();
    for (const auto id : {a, b}) {
        manager.GetWorld(id)->SpawnActor<WorldCountingActor>(ticks, destroyed)->DestroyAction = [&]() {
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
    test::ScopedWorldManager manager{&app, &renderer};
    const auto parent = manager.CreateWorld();
    EXPECT_EQ(manager.GetWorld(parent)->GetApplication().Get(), &app);
    auto* actor = manager.GetWorld(parent)->SpawnActor<WorldCountingActor>(ticks, destroyed);
    WorldId child;
    SceneId scene;
    ShapeId primitive;
    actor->Action = [&]() {
        child = manager.CreateWorld();
        manager.RequestRenderConnection(child, true);
        auto world = manager.GetWorld(child);
        world->SetTickEnabled(false);
        auto* mesh = world->SpawnActor()->AddComponent<StaticMeshComponent>();
        mesh->SetRelativeLocation({12, 0, 0});
        primitive = mesh->GetShapeId();
    };
    manager.Tick(0);
    manager.FinalizeWorldsGT();
    scene = *manager.GetWorld(child)->GetRenderSceneId();
    primitive = manager.GetWorld(child)->GetActors()[0]->FindComponent<StaticMeshComponent>()->GetShapeId();
    manager.CollectRenderUpdates();
    renderer.SealFrameGT(0);
    manager.Clear();
    EXPECT_FALSE(manager.GetWorld(child));
    EXPECT_EQ(destroyed, 1u);
    renderer.SealFrameGT(1);
    test::ConsumeFrame(renderer, 0);
    ASSERT_TRUE(renderer.GetSceneRT(scene));
    ASSERT_TRUE(renderer.GetSceneRT(scene)->GetStaticMesh(primitive));
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(scene)->GetStaticMesh(primitive)->LocalToWorld(0, 3), 12);
    test::ConsumeFrame(renderer, 1);
    EXPECT_FALSE(renderer.GetSceneRT(scene));
    const auto beforeCompletion = renderer.CreateSceneGT();
    EXPECT_NE(beforeCompletion.Index, scene.Index);
    test::CompleteFrame(renderer, 0);
    test::CompleteFrame(renderer, 1);
    const auto afterCompletion = renderer.CreateSceneGT();
    EXPECT_EQ(afterCompletion.Index, scene.Index);
    EXPECT_GT(afterCompletion.Generation, scene.Generation);
}

TEST(WorldManagerDeathTest, RejectsReentrantTickCollectionAndClear) {
    uint32_t ticks = 0, destroyed = 0;
    test::ScopedWorldManager manager;
    const auto id = manager.CreateWorld();
    auto* actor = manager.GetWorld(id)->SpawnActor<WorldCountingActor>(ticks, destroyed);
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

class ManagerCollectionProbe final : public RenderComponent {
public:
    explicit ManagerCollectionProbe(std::function<void()> action) : _action(std::move(action)) {}

protected:
    void CreateRenderState(SceneWriter&) override { MarkRenderStateDirty(); }
    void CollectRenderUpdates(SceneCapture&, RenderDirtyFlags) override { _action(); }

private:
    std::function<void()> _action;
};

TEST(WorldManagerDeathTest, RejectsCollectionMutationAndMissingRenderService) {
    test::ScopedWorldManager cpuOnly;
    const auto cpuWorld = cpuOnly.CreateWorld();
    EXPECT_EQ(cpuOnly.RequestRenderConnection(cpuWorld, true), LifecycleRequestResult::Invalid);
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorldManager manager{&app, &renderer};
    const auto id = manager.CreateWorld();
    manager.RequestRenderConnection(id, true);
    manager.FinalizeWorldsGT();
    auto* actor = manager.GetWorld(id)->SpawnActor();
    auto* probe = actor->AddComponent<ManagerCollectionProbe>([&]() { manager.DestroyWorld(id); });
    EXPECT_DEATH(manager.CollectRenderUpdates(), "");
    actor->RemoveComponent(probe);
    actor->AddComponent<ManagerCollectionProbe>([&]() { manager.RequestRenderConnection(id, false); });
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
    test::ScopedWorld world;
    auto* actor = world.SpawnActor();
    actor->AddComponent<RegistrationProbe>(registrations, unregistrations);
    auto* mesh = actor->AddComponent<StaticMeshComponent>();
    mesh->SetRelativeLocation({7, 0, 0});
    EXPECT_FALSE(mesh->GetShapeId().IsValid());
    world.SetTickEnabled(false);
    world.CollectRenderUpdates();
    const auto first = test::ConnectWorld(world, renderer);
    const auto oldPrimitive = mesh->GetShapeId();
    test::PrepareScene(world, renderer, 0);
    test::DisconnectWorld(world);
    EXPECT_FALSE(world.GetRenderSceneId());
    EXPECT_FALSE(mesh->GetShapeId().IsValid());
    mesh->SetRelativeLocation({19, 0, 0});
    const auto second = test::ConnectWorld(world, renderer);
    EXPECT_NE(first, second);
    const auto newPrimitive = mesh->GetShapeId();
    test::PrepareScene(world, renderer, 1);
    test::ConsumeFrame(renderer, 0);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(first)->GetStaticMesh(oldPrimitive)->LocalToWorld(0, 3), 7);
    test::ConsumeFrame(renderer, 1);
    EXPECT_FALSE(renderer.GetSceneRT(first));
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(second)->GetStaticMesh(newPrimitive)->LocalToWorld(0, 3), 19);
    EXPECT_EQ(registrations, 1u);
    EXPECT_EQ(unregistrations, 0u);
    test::CompleteFrame(renderer, 0);
    test::CompleteFrame(renderer, 1);
    world.DestroyActor(actor);
    world.FinalizeWorldGT();
    EXPECT_EQ(unregistrations, 1u);
}

TEST(WorldScenes, StandaloneWritersIsolateIdentitiesAndCoalesceUpdates) {
    Application app;
    RenderSystem renderer{&app, 2};
    const auto a = renderer.CreateSceneGT();
    const auto b = renderer.CreateSceneGT();
    auto wa = renderer.GetSceneWriterGT(a);
    auto wb = renderer.GetSceneWriterGT(b);
    const auto pa = wa->CreateShape();
    const auto pb = wb->CreateShape();
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
    EXPECT_EQ(batch.CreateShapes.size(), 1u);
    EXPECT_EQ(batch.MeshStates.size(), 1u);
    EXPECT_TRUE(batch.Transforms.empty());
    test::ConsumeFrame(renderer, 0);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(a)->GetStaticMesh(pa)->LocalToWorld(0, 3), 19);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(b)->GetStaticMesh(pb)->LocalToWorld(0, 3), 0);
    test::CompleteFrame(renderer, 0);
    renderer.DestroySceneGT(a);
    EXPECT_FALSE(renderer.GetSceneWriterGT(a));
    transform(0, 3) = 37;
    wb->SetTransform(pb, transform);
    renderer.SealFrameGT(1);
    test::ConsumeFrame(renderer, 1);
    EXPECT_FALSE(renderer.GetSceneRT(a));
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(b)->GetStaticMesh(pb)->LocalToWorld(0, 3), 37);
    const auto beforeCompletion = renderer.CreateSceneGT();
    EXPECT_NE(beforeCompletion.Index, a.Index);
    test::CompleteFrame(renderer, 1);
    const auto afterCompletion = renderer.CreateSceneGT();
    EXPECT_EQ(afterCompletion.Index, a.Index);
    EXPECT_GT(afterCompletion.Generation, a.Generation);
    EXPECT_FALSE(renderer.GetSceneRT(a));
    EXPECT_FALSE(renderer.GetSceneWriterGT(a));
}

TEST(WorldScenes, WritersSurviveRegistryCompactionAndIdentityReuse) {
    Application app;
    RenderSystem renderer{&app, 2};
    const auto removed = renderer.CreateSceneGT();
    const auto survivor = renderer.CreateSceneGT();
    auto* writer = renderer.GetSceneWriterGT(survivor).Get();
    const auto shape = writer->CreateShape();
    writer->SetStaticMesh(shape, {}, Eigen::Matrix4f::Identity());
    renderer.SealFrameGT(0);
    test::ConsumeFrame(renderer, 0);
    test::CompleteFrame(renderer, 0);

    renderer.DestroySceneGT(removed);
    renderer.SealFrameGT(0);
    renderer.SealFrameGT(1);
    EXPECT_EQ(renderer.GetFrameUpdatesRT(0).size(), 2u);
    EXPECT_EQ(renderer.GetFrameUpdatesRT(1).size(), 1u);
    test::ConsumeFrame(renderer, 0);
    test::CompleteFrame(renderer, 0);
    const auto replacement = renderer.CreateSceneGT();
    EXPECT_EQ(replacement.Index, removed.Index);
    EXPECT_GT(replacement.Generation, removed.Generation);
    EXPECT_EQ(renderer.GetSceneWriterGT(survivor).Get(), writer);
    test::ConsumeFrame(renderer, 1);
    test::CompleteFrame(renderer, 1);

    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    transform(0, 3) = 17;
    writer->SetTransform(shape, transform);
    renderer.SealFrameGT(0);
    EXPECT_EQ(renderer.GetFrameUpdatesRT(0).size(), 2u);
    test::ConsumeFrame(renderer, 0);
    EXPECT_FALSE(renderer.GetSceneRT(removed));
    EXPECT_TRUE(renderer.GetSceneRT(replacement));
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(survivor)->GetStaticMesh(shape)->LocalToWorld(0, 3), 17);
    test::CompleteFrame(renderer, 0);
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
                renderer.GetSceneWriterGT(scene)->CreateShape();
                renderer.DestroySceneGT(scene);
                removed.push_back(scene);
                renderer.SealFrameGT(flight);
            }
            for (uint32_t flight = 0; flight < count; ++flight) {
                test::ConsumeFrame(renderer, flight);
                EXPECT_FALSE(renderer.GetSceneRT(removed[flight]));
                EXPECT_TRUE(renderer.GetSceneRT(stable));
                test::CompleteFrame(renderer, flight);
                EXPECT_FALSE(renderer.GetSceneWriterGT(removed[flight]));
            }
        }
    }
}

TEST(WorldScenesDeathTest, ClaimedAndClosingScenesRejectExternalWrites) {
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    const auto attached = test::ConnectWorld(world, renderer);
    EXPECT_FALSE(renderer.GetSceneWriterGT(attached));
    EXPECT_DEATH(renderer.DestroySceneGT(attached), "");
    EXPECT_DEATH(renderer.OnShutdown(), "");
    EXPECT_EQ(test::ConnectWorld(world, renderer), attached);
    const auto standalone = renderer.CreateSceneGT();
    auto writer = renderer.GetSceneWriterGT(standalone);
    const auto primitive = writer->CreateShape();
    EXPECT_DEATH(writer->SetTransform(primitive, Eigen::Matrix4f::Identity()), "");
    renderer.DestroySceneGT(standalone);
    EXPECT_DEATH(writer->RemoveShape(primitive), "");
    EXPECT_DEATH(writer->CreateShape(), "");
}

}  // namespace
}  // namespace radray
