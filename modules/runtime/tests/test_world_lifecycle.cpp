#include "scene_test_support.h"

#include <gtest/gtest.h>
#include <functional>
#include <random>
#include <radray/runtime/components/spot_light_component.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>

namespace radray {
namespace {

struct LifecycleTrace {
    uint32_t Registered{0}, Unregistered{0}, Ticks{0}, Spawned{0}, Destroyed{0}, Freed{0}, CreatedRender{0}, DestroyedRender{0};
    uint32_t Gathered{0}, TransformNotified{0};
    RenderDirtyFlags LastDirty;
    vector<string> Events;
};

class LifecycleProbe final : public PrimitiveComponent {
public:
    explicit LifecycleProbe(LifecycleTrace& trace) : Trace(trace) {
        SetTickEnabled(true);
        SetTransformNotificationEnabled(true);
    }
    ~LifecycleProbe() noexcept override { ++Trace.Freed; }
    void OnRegister() override {
        EXPECT_EQ(GetRegistrationState(), ComponentRegistration::Registering);
        ++Trace.Registered;
        if (Register) Register(*this);
    }
    void OnUnregister() override {
        EXPECT_EQ(GetRegistrationState(), ComponentRegistration::Unregistering);
        ++Trace.Unregistered;
        if (Unregister) Unregister(*this);
    }
    void TickComponent(float) override {
        ++Trace.Ticks;
        if (Tick) Tick(*this);
    }
    LifecycleTrace& Trace;
    std::function<void(LifecycleProbe&)> Register, Unregister, Tick, CreateRender, DestroyRender, Transform;

protected:
    void OnRenderStateCreated() override {
        ++Trace.CreatedRender;
        PrimitiveComponent::OnRenderStateCreated();
        if (CreateRender) CreateRender(*this);
    }
    void OnRenderStateDestroyed() override {
        ++Trace.DestroyedRender;
        PrimitiveComponent::OnRenderStateDestroyed();
        if (DestroyRender) DestroyRender(*this);
    }
    void CollectPrimitiveUpdates(ShapeCapture& writer, RenderDirtyFlags dirty) override {
        ++Trace.Gathered;
        Trace.LastDirty = dirty;
        if (dirty.HasFlag(RenderDirtyFlag::State))
            writer.SetStaticMesh({}, GetWorldMatrix());
        else if (dirty.HasFlag(RenderDirtyFlag::Transform))
            writer.SetTransform(GetWorldMatrix());
    }
    void OnTransformChanged() override {
        ++Trace.TransformNotified;
        PrimitiveComponent::OnTransformChanged();
        if (Transform) Transform(*this);
    }
};

class LifecycleActor final : public Actor {
public:
    explicit LifecycleActor(LifecycleTrace& trace) : Trace(trace) { SetTickEnabled(true); }
    ~LifecycleActor() noexcept override { ++Trace.Freed; }
    void Tick(float) override {
        ++Trace.Ticks;
        if (Update) Update(*this);
    }
    LifecycleTrace& Trace;
    std::function<void(LifecycleActor&)> Spawn, Destroy, Update;

protected:
    void OnSpawned() override {
        ++Trace.Spawned;
        if (Spawn) Spawn(*this);
    }
    void OnDestroyed() override {
        ++Trace.Destroyed;
        if (Destroy) Destroy(*this);
    }
};

TEST(WorldLifecycle, ImmediateCreationParticipatesInTheNextGlobalRound) {
    LifecycleTrace actors, components;
    test::ScopedWorldManager manager;
    const auto id = manager.CreateWorld();
    auto world = manager.GetWorld(id);
    auto draft = make_unique<LifecycleActor>(actors);
    draft->Spawn = [&](auto& actor) {
        auto* component = actor.template AddComponent<LifecycleProbe>(components);
        component->SetRelativeLocation({1, 2, 3});
        EXPECT_EQ(component->GetOwner().Get(), &actor);
        EXPECT_TRUE(component->IsRegistered());
    };
    auto* actor = world->SpawnActor(std::move(draft));
    EXPECT_EQ(world->FindLive(actor->GetId()).Get(), actor);
    EXPECT_EQ(actors.Spawned, 1u);
    EXPECT_EQ(components.Registered, 1u);
    manager.Tick(0);
    EXPECT_EQ(actors.Ticks, 1u);
    EXPECT_EQ(components.Ticks, 1u);
}

TEST(WorldLifecycle, TickAppendCanReallocateActorAndComponentOwners) {
    LifecycleTrace actorTrace, componentTrace;
    test::ScopedWorld world;
    auto* actor = world.SpawnActor<LifecycleActor>(actorTrace);
    actor->Update = [&](auto& self) {
        if (actorTrace.Ticks != 1) return;
        for (int i = 0; i < 512; ++i) {
            world.SpawnActor<LifecycleActor>(actorTrace);
            self.template AddComponent<LifecycleProbe>(componentTrace);
        }
    };
    world.Tick(0);
    EXPECT_EQ(actorTrace.Ticks, 1u);
    EXPECT_EQ(componentTrace.Ticks, 0u);
    EXPECT_EQ(componentTrace.Registered, 512u);
    world.Tick(0);
    EXPECT_EQ(actorTrace.Ticks, 514u);
    EXPECT_EQ(componentTrace.Ticks, 512u);
}

TEST(WorldLifecycle, CrossActorFirstTickDoesNotDependOnTraversalOrder) {
    for (bool sourceFirst : {false, true}) {
        LifecycleTrace sourceTrace, destinationTrace, componentTrace;
        test::ScopedWorld world;
        LifecycleActor* source;
        LifecycleActor* destination;
        if (sourceFirst) {
            source = world.SpawnActor<LifecycleActor>(sourceTrace);
            destination = world.SpawnActor<LifecycleActor>(destinationTrace);
        } else {
            destination = world.SpawnActor<LifecycleActor>(destinationTrace);
            source = world.SpawnActor<LifecycleActor>(sourceTrace);
        }
        source->Update = [&](auto&) { if (sourceTrace.Ticks == 1) destination->AddComponent<LifecycleProbe>(componentTrace); };
        world.Tick(0);
        EXPECT_EQ(componentTrace.Registered, 1u);
        EXPECT_EQ(componentTrace.Ticks, 0u);
        world.Tick(0);
        EXPECT_EQ(componentTrace.Ticks, 1u);
    }
}

TEST(WorldLifecycle, CrossWorldAndNewWorldUseTheSameEpoch) {
    LifecycleTrace sourceTrace, existingTrace, newTrace;
    test::ScopedWorldManager manager;
    const auto a = manager.CreateWorld();
    const auto b = manager.CreateWorld();
    auto* source = manager.GetWorld(a)->SpawnActor<LifecycleActor>(sourceTrace);
    WorldId created;
    source->Update = [&](auto&) {
        if (sourceTrace.Ticks != 1) return;
        manager.GetWorld(b)->SpawnActor<LifecycleActor>(existingTrace);
        created = manager.CreateWorld();
        manager.GetWorld(created)->SpawnActor<LifecycleActor>(newTrace);
    };
    manager.Tick(0);
    EXPECT_EQ(existingTrace.Ticks, 0u);
    EXPECT_EQ(newTrace.Ticks, 0u);
    manager.Tick(0);
    EXPECT_EQ(existingTrace.Ticks, 1u);
    EXPECT_EQ(newTrace.Ticks, 1u);
    manager.GetWorld(b)->SetTickEnabled(false);
    manager.Tick(0);
    manager.GetWorld(b)->SetTickEnabled(true);
    manager.Tick(0);
    EXPECT_EQ(existingTrace.Ticks, 2u);
}

TEST(WorldLifecycle, DraftRegistrationAndRecursiveAppendAreExactlyOnce) {
    LifecycleTrace actors, components;
    test::ScopedWorld world;
    auto draft = make_unique<LifecycleActor>(actors);
    auto* first = draft->AddComponent<LifecycleProbe>(components);
    draft->AddComponent<LifecycleProbe>(components);
    first->Register = [&](auto& component) {
        for (int i = 0; i < 128; ++i) component.GetOwner()->template AddComponent<LifecycleProbe>(components);
    };
    world.Tick(0);
    world.Tick(0);
    world.SpawnActor(std::move(draft));
    EXPECT_EQ(components.Registered, 130u);
    world.Tick(0);
    EXPECT_EQ(components.Ticks, 130u);
    world.ShutdownWorld();
    EXPECT_EQ(components.Unregistered, 130u);
}

TEST(WorldLifecycle, SelfDestructionFinishesTheActiveRegistrationAndPairsOnlyStartedHooks) {
    for (bool inSpawn : {false, true}) {
        LifecycleTrace actorTrace, componentTrace;
        test::ScopedWorld world;
        auto draft = make_unique<LifecycleActor>(actorTrace);
        auto* first = draft->AddComponent<LifecycleProbe>(componentTrace);
        draft->AddComponent<LifecycleProbe>(componentTrace);
        if (inSpawn)
            draft->Spawn = [&](auto& actor) { world.DestroyActor(&actor); };
        else
            first->Register = [&](auto& component) { world.DestroyActor(component.GetOwner().Get()); };
        auto* actor = world.SpawnActor(std::move(draft));
        EXPECT_EQ(actor->GetLifecycle(), ObjectLifecycle::PendingDestroy);
        EXPECT_EQ(actorTrace.Freed, 0u);
        EXPECT_EQ(componentTrace.Registered, inSpawn ? 2u : 1u);
        world.Tick(0);
        EXPECT_EQ(actorTrace.Ticks, 0u);
        EXPECT_EQ(componentTrace.Ticks, 0u);
        world.FinalizeWorldGT();
        EXPECT_EQ(actorTrace.Destroyed, inSpawn ? 1u : 0u);
        EXPECT_EQ(componentTrace.Unregistered, inSpawn ? 2u : 1u);
        EXPECT_EQ(actorTrace.Freed, 1u);
    }
}

TEST(WorldLifecycle, PendingStopsRemainingDispatchButKeepsTheCurrentStackAlive) {
    LifecycleTrace actorTrace, componentTrace;
    test::ScopedWorld world;
    auto* actor = world.SpawnActor<LifecycleActor>(actorTrace);
    actor->AddComponent<LifecycleProbe>(componentTrace);
    actor->Update = [&](auto& self) {
        for (int i = 0; i < 100; ++i) EXPECT_EQ(world.DestroyActor(&self), i == 0 ? LifecycleRequestResult::Accepted : LifecycleRequestResult::AlreadyPending);
        EXPECT_FALSE(world.FindLive(self.GetId()));
        EXPECT_EQ(actorTrace.Freed, 0u);
        actorTrace.Events.push_back("after-request");
    };
    world.Tick(0);
    EXPECT_EQ(actorTrace.Ticks, 1u);
    EXPECT_EQ(componentTrace.Ticks, 0u);
    EXPECT_EQ(actorTrace.Freed, 0u);
    world.FinalizeWorldGT();
    EXPECT_EQ(actorTrace.Freed, 1u);
    EXPECT_EQ(componentTrace.Unregistered, 1u);
    EXPECT_EQ(actorTrace.Events, vector<string>{"after-request"});
}

TEST(WorldLifecycle, ComponentCanDestroyItselfSiblingActorOrWorld) {
    for (int target = 0; target < 4; ++target) {
        LifecycleTrace componentTrace;
        test::ScopedWorldManager manager;
        const auto id = manager.CreateWorld();
        auto* actor = manager.GetWorld(id)->SpawnActor();
        auto* first = actor->AddComponent<LifecycleProbe>(componentTrace);
        auto* second = actor->AddComponent<LifecycleProbe>(componentTrace);
        first->Tick = [&](auto& self) {
            if (target == 0) actor->RemoveComponent(&self);
            if (target == 1) actor->RemoveComponent(second);
            if (target == 2) manager.GetWorld(id)->DestroyActor(actor);
            if (target == 3) manager.DestroyWorld(id);
            EXPECT_EQ(componentTrace.Freed, 0u);
        };
        manager.Tick(0);
        EXPECT_EQ(componentTrace.Ticks, target == 0 ? 2u : 1u);
        EXPECT_EQ(componentTrace.Freed, 0u);
        manager.FinalizeWorldsGT();
        EXPECT_EQ(componentTrace.Freed, target < 2 ? 1u : 2u);
    }
}

TEST(WorldLifecycle, ParentDestructionCoversQueuedChildrenAndRejectsStaleIdentities) {
    LifecycleTrace actorTrace, componentTrace;
    test::ScopedWorld world;
    auto* actor = world.SpawnActor<LifecycleActor>(actorTrace);
    auto* component = actor->AddComponent<LifecycleProbe>(componentTrace);
    const auto actorId = actor->GetId();
    const auto componentId = component->GetId();
    actor->RemoveComponent(component);
    world.DestroyActor(actor);
    world.FinalizeWorldGT();
    EXPECT_EQ(actorTrace.Freed, 1u);
    EXPECT_EQ(componentTrace.Freed, 1u);
    auto* replacement = world.SpawnActor();
    EXPECT_EQ(replacement->GetId().Index, actorId.Index);
    EXPECT_GT(replacement->GetId().Generation, actorId.Generation);
    replacement->AddComponent<LifecycleProbe>(componentTrace);
    EXPECT_FALSE(world.FindLive(actorId));
    EXPECT_FALSE(world.FindLive(componentId));
    EXPECT_EQ(world.DestroyActor(actorId), LifecycleRequestResult::Invalid);
}

TEST(WorldLifecycle, DestructionCallbacksSeeCompactedSurvivorsAndMaySpawnDrops) {
    LifecycleTrace deleted, survivors, drops;
    test::ScopedWorld world;
    vector<ActorId> survivorIds;
    for (int i = 0; i < 256; ++i) {
        auto* actor = world.SpawnActor<LifecycleActor>((i % 2) ? survivors : deleted);
        if (i % 2)
            survivorIds.push_back(actor->GetId());
        else {
            actor->Destroy = [&](auto&) {
                for (size_t j = 0; j < survivorIds.size(); ++j) EXPECT_EQ(world.GetActors()[j]->GetId(), survivorIds[j]);
                world.SpawnActor<LifecycleActor>(drops);
            };
            world.DestroyActor(actor);
        }
    }
    world.FinalizeWorldGT();
    EXPECT_EQ(deleted.Freed, 128u);
    EXPECT_EQ(drops.Spawned, 128u);
    EXPECT_EQ(world.GetActors().size(), 256u);
}

TEST(WorldLifecycle, DestructionRequestsMadeByCallbacksWaitForTheNextBatch) {
    LifecycleTrace oldTrace, nextTrace;
    test::ScopedWorld world;
    auto* old = world.SpawnActor<LifecycleActor>(oldTrace);
    auto* next = world.SpawnActor<LifecycleActor>(nextTrace);
    old->Destroy = [&](auto&) {
        world.DestroyActor(next);
        auto* newActor = world.SpawnActor<LifecycleActor>(nextTrace);
        world.DestroyActor(newActor);
    };
    world.DestroyActor(old);
    world.FinalizeWorldGT();
    EXPECT_EQ(nextTrace.Freed, 0u);
    EXPECT_FALSE(next->IsLive());
    world.Tick(0);
    EXPECT_EQ(nextTrace.Ticks, 0u);
    world.FinalizeWorldGT();
    EXPECT_EQ(nextTrace.Freed, 2u);
}

TEST(WorldLifecycle, PausedCpuOnlyWorldStillCommitsDestruction) {
    LifecycleTrace trace;
    test::ScopedWorldManager manager;
    const auto id = manager.CreateWorld();
    auto world = manager.GetWorld(id);
    world->SetTickEnabled(false);
    world->DestroyActor(world->SpawnActor<LifecycleActor>(trace));
    manager.FinalizeWorldsGT();
    EXPECT_EQ(trace.Freed, 1u);
    EXPECT_TRUE(manager.GetWorld(id));
}

TEST(WorldLifecycle, InitialParentIsVisibleDuringRegistrationAndAppendSafeDuringNotification) {
    LifecycleTrace trace;
    test::ScopedWorld world;
    auto* actor = world.SpawnActor();
    auto* parent = actor->AddComponent<LifecycleProbe>(trace);
    auto child = make_unique<LifecycleProbe>(trace);
    child->Register = [&](auto& value) { EXPECT_EQ(value.GetAttachParent().Get(), parent); };
    actor->AddComponent(std::move(child), parent, AttachmentRule::KeepLocal);
    parent->Transform = [&](auto&) {
        for (int i = 0; i < 128; ++i) actor->AddSceneComponent<LifecycleProbe>(parent, AttachmentRule::KeepLocal, trace);
    };
    parent->SetRelativeLocation({3, 0, 0});
    world.FinalizeWorldGT();
    EXPECT_EQ(parent->GetAttachChildren().size(), 129u);
    for (auto* value : parent->GetAttachChildren()) EXPECT_FLOAT_EQ(value->GetWorldLocation().x(), 3);
}

TEST(WorldLifecycle, ReparentIsDeferredAndRejectsCyclesCrossWorldAndShear) {
    test::ScopedWorldManager manager;
    const auto a = manager.CreateWorld(), b = manager.CreateWorld();
    auto* actor = manager.GetWorld(a)->SpawnActor();
    auto* parent = actor->AddComponent<SceneComponent>();
    auto* child = actor->AddSceneComponent<SceneComponent>(parent, AttachmentRule::KeepLocal);
    auto* other = actor->AddComponent<SceneComponent>();
    auto* foreign = manager.GetWorld(b)->SpawnActor()->AddComponent<SceneComponent>();
    parent->SetRelativeLocation({2, 0, 0});
    other->SetRelativeLocation({5, 0, 0});
    EXPECT_EQ(parent->RequestReparent(child), LifecycleRequestResult::Invalid);
    EXPECT_EQ(child->RequestReparent(foreign), LifecycleRequestResult::Invalid);
    EXPECT_EQ(child->RequestReparent(other, AttachmentRule::KeepWorld), LifecycleRequestResult::Accepted);
    EXPECT_EQ(child->GetAttachParent().Get(), parent);
    manager.FinalizeWorldsGT();
    EXPECT_EQ(child->GetAttachParent().Get(), other);
    EXPECT_FLOAT_EQ(child->GetWorldLocation().x(), 2);
    other->SetRelativeScale({2, 1, 1});
    child->SetRelativeRotation(Eigen::Quaternionf{Eigen::AngleAxisf{0.5f, Eigen::Vector3f::UnitZ()}});
    EXPECT_EQ(child->RequestReparent(nullptr, AttachmentRule::KeepWorld), LifecycleRequestResult::Invalid);
    EXPECT_EQ(child->GetAttachParent().Get(), other);
}

TEST(WorldLifecycle, ParentRemovalDetachesOtherActorsChildrenWithKeepLocal) {
    test::ScopedWorld world;
    auto* owner = world.SpawnActor();
    auto* survivor = world.SpawnActor();
    auto* parent = owner->AddComponent<SceneComponent>();
    auto* child = survivor->AddSceneComponent<SceneComponent>(parent, AttachmentRule::KeepLocal);
    parent->SetRelativeLocation({8, 0, 0});
    child->SetRelativeLocation({2, 0, 0});
    world.DestroyActor(owner);
    world.FinalizeWorldGT();
    EXPECT_FALSE(child->GetAttachParent());
    EXPECT_TRUE(child->IsLive());
    EXPECT_FLOAT_EQ(child->GetWorldLocation().x(), 2);
}

TEST(WorldLifecycle, ConnectingCallbacksAppendExactlyOnceAndRequestNextBatchDisconnect) {
    LifecycleTrace trace;
    Application app;
    RenderSystem render{&app, 2};
    test::ScopedWorld world;
    auto* actor = world.SpawnActor();
    auto* first = actor->AddComponent<LifecycleProbe>(trace);
    first->CreateRender = [&](auto&) {
        EXPECT_EQ(world.GetRenderConnectionState(), RenderConnectionState::Connecting);
        EXPECT_EQ(world.GetRequestedRenderConnection().Get(), &render);
        world.RequestRenderConnection(nullptr);
        EXPECT_FALSE(world.GetRequestedRenderConnection());
        for (int i = 0; i < 64; ++i) world.SpawnActor()->AddComponent<LifecycleProbe>(trace);
        for (int i = 0; i < 64; ++i) actor->AddComponent<LifecycleProbe>(trace);
    };
    world.RequestRenderConnection(&render);
    world.FinalizeWorldGT();
    EXPECT_EQ(trace.CreatedRender, 129u);
    EXPECT_EQ(trace.Registered, 129u);
    EXPECT_TRUE(world.GetRenderSceneId());
    world.FinalizeWorldGT();
    EXPECT_FALSE(world.GetRenderSceneId());
    EXPECT_EQ(trace.DestroyedRender, 129u);
}

TEST(WorldLifecycle, DisconnectCallbacksCanCreateButCannotRejoinTheDyingScene) {
    LifecycleTrace original, added;
    Application app;
    RenderSystem render{&app, 2};
    test::ScopedWorld world;
    auto* first = world.SpawnActor()->AddComponent<LifecycleProbe>(original);
    first->DestroyRender = [&](auto&) {
        if (world.GetLifecycle() != ObjectLifecycle::Live) return;
        EXPECT_EQ(world.GetRenderConnectionState(), RenderConnectionState::Disconnecting);
        EXPECT_EQ(world.GetRequestedRenderConnection().Get(), &render);
        world.SpawnActor()->AddComponent<LifecycleProbe>(added);
        world.RequestRenderConnection(&render);
    };
    const auto previous = test::ConnectWorld(world, render);
    test::DisconnectWorld(world);
    EXPECT_EQ(added.Registered, 1u);
    EXPECT_EQ(added.CreatedRender, 0u);
    EXPECT_FALSE(world.GetRenderSceneId());
    EXPECT_EQ(world.GetRequestedRenderConnection().Get(), &render);
    world.FinalizeWorldGT();
    EXPECT_EQ(added.CreatedRender, 1u);
    EXPECT_NE(*world.GetRenderSceneId(), previous);
}

TEST(WorldLifecycle, ReconnectSurvivesCoalescingAndPendingSourcesAreNotCollected) {
    LifecycleTrace trace;
    Application app;
    RenderSystem render{&app, 2};
    test::ScopedWorld world;
    auto* component = world.SpawnActor()->AddComponent<LifecycleProbe>(trace);
    const auto first = test::ConnectWorld(world, render);
    world.RequestReconnect();
    world.RequestRenderConnection(&render);
    world.FinalizeWorldGT();
    EXPECT_NE(*world.GetRenderSceneId(), first);
    EXPECT_EQ(trace.CreatedRender, 2u);
    EXPECT_EQ(trace.DestroyedRender, 1u);
    world.DestroyActor(component->GetOwner().Get());
    world.CollectRenderUpdates();
    render.SealFrameGT(0);
    EXPECT_TRUE(test::SceneBatch(render, *world.GetRenderSceneId(), 0).MeshStates.empty());
}

TEST(WorldLifecycleDeathTest, RejectsCreationAndDriversDuringInvalidPhases) {
    LifecycleTrace trace;
    test::ScopedWorld world;
    auto* actor = world.SpawnActor<LifecycleActor>(trace);
    actor->Update = [&](auto&) { world.FinalizeWorldGT(); };
    EXPECT_DEATH(world.Tick(0), "");
    actor->Update = [&](auto&) { world.Tick(0); };
    EXPECT_DEATH(world.Tick(0), "");
    world.DestroyActor(actor);
    EXPECT_DEATH(actor->AddComponent<SceneComponent>(), "");
    world.FinalizeWorldGT();
    world.ShutdownWorld();
    EXPECT_DEATH(world.SpawnActor(), "");
}

TEST(WorldLifecycle, CreatedThenPendingInADestroyHookNeverPublishesAnEmptyPrimitive) {
    LifecycleTrace oldTrace, newTrace;
    Application app;
    RenderSystem renderer{&app, 2};
    test::ScopedWorld world;
    const auto sceneId = test::ConnectWorld(world, renderer);
    auto* old = world.SpawnActor<LifecycleActor>(oldTrace);
    old->Destroy = [&](auto&) {
        auto* created = world.SpawnActor<LifecycleActor>(newTrace);
        created->AddComponent<PrimitiveComponent>();
        world.DestroyActor(created);
    };
    world.DestroyActor(old);
    world.FinalizeWorldGT();
    EXPECT_EQ(newTrace.Freed, 0u);
    world.CollectRenderUpdates();
    renderer.SealFrameGT(0);
    EXPECT_TRUE(test::SceneBatch(renderer, sceneId, 0).CreateShapes.empty());
    EXPECT_TRUE(test::SceneBatch(renderer, sceneId, 0).MeshStates.empty());
    EXPECT_EQ(test::SceneBatch(renderer, sceneId, 0).CreateTransforms.size(), 1u);
    test::ConsumeFrame(renderer, 0);
    test::CompleteFrame(renderer, 0);
    test::PrepareScene(world, renderer, 0);
    EXPECT_EQ(newTrace.Freed, 1u);
    EXPECT_TRUE(test::SceneBatch(renderer, sceneId, 0).RemoveShapes.empty());
    EXPECT_EQ(test::SceneBatch(renderer, sceneId, 0).RemoveTransforms.size(), 1u);
}

TEST(WorldLifecycle, LightUsesOneTypedUpdateAndNoMeshState) {
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    const auto sceneId = test::ConnectWorld(world, renderer);
    auto* actor = world.SpawnActor();
    auto* light = actor->AddComponent<SpotLightComponent>();
    for (int i = 0; i < 100; ++i) {
        light->SetIntensity(float(i));
        light->SetAttenuationRadius(float(i));
        light->SetRelativeLocation({float(i), 2, 3});
    }
    test::PrepareScene(world, renderer, 0);
    const auto& batch = test::SceneBatch(renderer, sceneId, 0);
    ASSERT_EQ(batch.Lights.Count(), 1u);
    ASSERT_EQ(batch.Lights.SpotLights.Size(), 1u);
    EXPECT_TRUE(batch.MeshStates.empty());
    EXPECT_TRUE(batch.Transforms.empty());
    EXPECT_FLOAT_EQ(batch.Lights.SpotLights.Data[0].Common.Intensity, 99);
    EXPECT_EQ(batch.Lights.SpotLights.Data[0].Common.Transform, light->GetSceneTransformId());
    const auto id = batch.Lights.SpotLights.Ids[0];
    test::ConsumeFrame(renderer, 0);
    EXPECT_TRUE(renderer.GetSceneRT(sceneId)->GetLights().GetSpotLight(id));
    EXPECT_EQ(renderer.GetSceneRT(sceneId)->GetLights().SpotLights.Size(), 1u);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(sceneId)->GetLights().GetSpotLight(id)->Point.Position.x(), 99);
    test::CompleteFrame(renderer, 0);
    light->SetIntensity(99);
    light->SetRelativeLocation({99, 2, 3});
    test::PrepareScene(world, renderer, 0);
    EXPECT_TRUE(test::SceneBatch(renderer, sceneId, 0).Empty());
    test::ConsumeFrame(renderer, 0);
    test::CompleteFrame(renderer, 0);
    actor->RemoveComponent(light);
    test::PrepareScene(world, renderer, 0);
    test::ConsumeFrame(renderer, 0);
    EXPECT_TRUE(renderer.GetSceneRT(sceneId)->GetLights().SpotLights.Empty());
    test::CompleteFrame(renderer, 0);
}

class IllegalCollect final : public PrimitiveComponent {
public:
    std::function<void()> SideEffect;
    void CollectPrimitiveUpdates(ShapeCapture&, RenderDirtyFlags) override { SideEffect(); }
};
TEST(WorldLifecycleDeathTest, CollectionRejectsFlushAndSchedulerSideEffects) {
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    test::ConnectWorld(world, renderer);
    auto* component = world.SpawnActor()->AddComponent<IllegalCollect>();
    component->SideEffect = [&] { renderer.SealFrameGT(0); };
    EXPECT_DEATH(world.CollectRenderUpdates(), "");
    component->SideEffect = [&] { renderer.CreateSceneGT(); };
    EXPECT_DEATH(world.CollectRenderUpdates(), "");
    component->SideEffect = [&] { app.GetScheduler().Pump(); };
    EXPECT_DEATH(world.CollectRenderUpdates(), "");
    component->SideEffect = [&] { world.DestroyActor(component->GetOwner().Get()); };
    EXPECT_DEATH(world.CollectRenderUpdates(), "");
    component->SideEffect = [&] { component->SetRelativeLocation({1, 0, 0}); };
    EXPECT_DEATH(world.CollectRenderUpdates(), "");
    component->SideEffect = [&] { component->MarkRenderStateDirty(); };
    EXPECT_DEATH(world.CollectRenderUpdates(), "");
    component->SideEffect = [&] { world.CollectRenderUpdates(); };
    EXPECT_DEATH(world.CollectRenderUpdates(), "");
}

TEST(WorldLifecycle, SchedulerFreezesItsBatchAndRevalidatesCanceledWaiters) {
    ApplicationScheduler scheduler;
    TaskScope first, canceled, added;
    uint32_t firstRuns = 0, canceledRuns = 0, addedRuns = 0;
    auto addedTask = [&]() -> task<void> { co_await scheduler.SwitchTo(); ++addedRuns; };
    auto canceledTask = [&]() -> task<void> { co_await scheduler.SwitchTo(); ++canceledRuns; };
    auto firstTask = [&]() -> task<void> {
        co_await scheduler.SwitchTo();
        ++firstRuns;
        canceled.RequestStop();
        added.Spawn(addedTask());
        co_await scheduler.SwitchTo();
        ++firstRuns;
    };
    first.Spawn(firstTask());
    canceled.Spawn(canceledTask());
    scheduler.Pump();
    EXPECT_EQ(firstRuns, 1u);
    EXPECT_EQ(canceledRuns, 0u);
    EXPECT_EQ(addedRuns, 0u);
    scheduler.Pump();
    EXPECT_EQ(firstRuns, 2u);
    EXPECT_EQ(addedRuns, 1u);
    scheduler.BeginStopping();
    added.Spawn(addedTask());
    EXPECT_EQ(addedRuns, 1u);
}

TEST(WorldLifecycle, MissingCompletionFacilityKeepsDeferredOwnerUntilTerminalCleanup) {
    uint32_t destroyed = 0;
    struct Payload {
        uint32_t* Count;
        ~Payload() noexcept { ++*Count; }
    };
    {
        AssetManager assets;
        assets.DeferDestroy(make_unique<Payload>(&destroyed));
        assets.Pump();
        EXPECT_EQ(destroyed, 0u);
    }
    EXPECT_EQ(destroyed, 1u);
}

TEST(WorldLifecycleDeathTest, StoppingRejectsAssetProduction) {
    AssetManager assets;
    assets.BeginStopping();
    EXPECT_DEATH(assets.Load(AssetId{}), "");
}

TEST(WorldLifecycleDeathTest, DraftCannotSmuggleAnExternalHierarchyIntoAWorld) {
    test::ScopedWorld world;
    auto external = make_unique<Actor>();
    auto* parent = external->AddComponent<SceneComponent>();
    auto draft = make_unique<Actor>();
    draft->AddComponent<SceneComponent>()->AttachTo(parent);
    EXPECT_DEATH(world.SpawnActor(std::move(draft)), "");
    auto orphan = make_unique<SceneComponent>();
    orphan->AttachTo(parent);
    auto* live = world.SpawnActor();
    EXPECT_DEATH(live->AddComponent(std::move(orphan)), "");
    EXPECT_TRUE(live->GetOwnedComponents().empty());
}

TEST(WorldLifecycle, DraftGetsItsFirstTickEpochWhenItJoinsDuringTick) {
    LifecycleTrace trace, triggerTrace;
    test::ScopedWorld world;
    auto draft = make_unique<LifecycleActor>(trace);
    for (int i = 0; i < 10; ++i) world.Tick(0);
    auto* trigger = world.SpawnActor<LifecycleActor>(triggerTrace);
    trigger->Update = [&](auto&) { if (draft) world.SpawnActor(std::move(draft)); };
    world.Tick(0);
    EXPECT_EQ(trace.Ticks, 0u);
    world.Tick(0);
    EXPECT_EQ(trace.Ticks, 1u);
}

TEST(WorldLifecycle, PartialBootstrapShutsDownWithoutAPublishedFlight) {
    LifecycleTrace trace;
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorldManager manager{&app, &renderer};
    const auto id = manager.CreateWorld();
    manager.GetWorld(id)->SpawnActor<LifecycleActor>(trace)->AddComponent<PrimitiveComponent>();
    manager.RequestRenderConnection(id, true);
    manager.FinalizeWorldsGT();
    manager.Shutdown();
    renderer.BeginStoppingGT();
    renderer.AbandonUnpublishedFramesGT();
    EXPECT_EQ(trace.Destroyed, 1u);
    EXPECT_EQ(trace.Freed, 1u);
    EXPECT_EQ(renderer.GetUpdateSequence(0), 0u);
    EXPECT_EQ(renderer.GetFrameSerial(0), 0u);
    EXPECT_TRUE(renderer.GetFrameUpdatesRT(0).empty());
}

TEST(WorldLifecycle, RandomizedSceneDeliveryMatchesLogicalValuesAcrossDelayedCompletions) {
    Application app;
    RenderSystem renderer{&app, 3};
    test::ScopedWorld world;
    std::mt19937 random{0x74c620u};
    struct Model {
        ActorId Actor;
        ComponentId Component;
        Eigen::Vector3f Position;
        bool Pending;
    };
    struct Expected {
        ShapeId Id;
        Eigen::Vector3f Position;
    };
    struct Packet {
        uint32_t Flight;
        uint64_t Serial;
        std::optional<SceneId> Scene;
        vector<Expected> Values;
    };
    vector<Model> model;
    vector<Packet> pending;
    array<bool, 3> writable{true, true, true};
    bool connected = false;
    uint32_t completedPackets = 0;
    const auto consume = [&] {
        auto packet = std::move(pending.front());
        pending.erase(pending.begin());
        renderer.ConsumeRenderUpdates(packet.Flight, packet.Serial);
        if (packet.Scene) {
            const auto scene = renderer.GetSceneRT(*packet.Scene);
            ASSERT_TRUE(scene);
            EXPECT_EQ(scene->GetStaticMeshes().size(), packet.Values.size());
            for (const auto& value : packet.Values) {
                const auto view = scene->GetStaticMesh(value.Id);
                ASSERT_TRUE(view);
                EXPECT_TRUE((view->LocalToWorld.block<3, 1>(0, 3)).isApprox(value.Position));
            }
        }
        test::CompleteFrame(renderer, packet.Flight, (packet.Serial % 2) == 0);
        EXPECT_TRUE(renderer.GetFrameUpdatesRT(packet.Flight).empty());
        ++completedPackets;
        writable[packet.Flight] = true;
    };
    for (uint32_t step = 0; step < 1500; ++step) {
        SCOPED_TRACE(step);
        const auto action = random() % 7;
        if (action == 0 || model.empty()) {
            auto* actor = world.SpawnActor();
            auto* component = actor->AddComponent<StaticMeshComponent>();
            model.push_back({actor->GetId(), component->GetId(), Eigen::Vector3f::Zero(), false});
        } else if (action <= 2) {
            auto& object = model[random() % model.size()];
            if (action == 1) {
                world.DestroyActor(object.Actor);
                object.Pending = true;
            } else if (!object.Pending) {
                object.Position = {float(step), float(step % 13), 0};
                dynamic_cast<StaticMeshComponent*>(world.FindLive(object.Component).Get())->SetRelativeLocation(object.Position);
            }
        } else if (action == 3) {
            connected = !connected;
            world.RequestRenderConnection(connected ? &renderer : nullptr);
        } else if (action == 4 && connected)
            world.RequestReconnect();
        else if (action == 5)
            world.SetTickEnabled((step % 2) == 0);
        else
            world.Tick(0);
        world.FinalizeWorldGT();
        std::erase_if(model, [](const auto& object) { return object.Pending; });
        EXPECT_EQ(world.GetRenderSceneId().has_value(), connected);
        if (pending.size() == 3) consume();
        const auto free = std::find(writable.begin(), writable.end(), true);
        const auto flight = uint32_t(free - writable.begin());
        Packet packet{flight, 0, world.GetRenderSceneId(), {}};
        if (connected) {
            for (const auto& object : model) {
                auto* component = dynamic_cast<StaticMeshComponent*>(world.FindLive(object.Component).Get());
                packet.Values.push_back({component->GetShapeId(), object.Position});
            }
        }
        world.CollectRenderUpdates();
        renderer.SealFrameGT(flight);
        renderer.PublishFrameGT(flight);
        packet.Serial = renderer.GetUpdateSequence(flight);
        writable[flight] = false;
        pending.push_back(std::move(packet));
        if (random() % 3 == 0) consume();
    }
    while (!pending.empty()) consume();
    EXPECT_EQ(completedPackets, 1500u);
    EXPECT_TRUE(std::all_of(writable.begin(), writable.end(), [](bool value) { return value; }));
}

TEST(WorldLifecycle, DeepHierarchyKeepsImmediateValuesAndSeparatesNotificationFromCapture) {
    LifecycleTrace trace;
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    const auto scene = test::ConnectWorld(world, renderer);
    auto* actor = world.SpawnActor();
    auto* root = actor->AddComponent<LifecycleProbe>(trace);
    auto* leaf = root;
    for (int i = 1; i < 256; ++i) leaf = actor->AddSceneComponent<LifecycleProbe>(leaf, AttachmentRule::KeepLocal, trace);
    test::PrepareScene(world, renderer, 0);
    test::ConsumeFrame(renderer, 0);
    test::CompleteFrame(renderer, 0);
    const auto notificationsBefore = trace.TransformNotified;
    const auto gatheredBefore = trace.Gathered;
    for (int i = 1; i <= 100; ++i) {
        root->SetRelativeLocation({float(i), 0, 0});
        EXPECT_FLOAT_EQ(leaf->GetWorldMatrix()(0, 3), float(i));
    }
    EXPECT_EQ(trace.TransformNotified, notificationsBefore);
    EXPECT_EQ(trace.Gathered, gatheredBefore);
    test::PrepareScene(world, renderer, 0);
    EXPECT_EQ(trace.TransformNotified - notificationsBefore, 256u);
    EXPECT_EQ(trace.Gathered - gatheredBefore, 256u);
    EXPECT_EQ(test::SceneBatch(renderer, scene, 0).Transforms.size(), 256u);
    EXPECT_TRUE(test::SceneBatch(renderer, scene, 0).MeshStates.empty());
}

TEST(WorldLifecycle, UncachedQueriesAndLocalDeliveryPreserveAffineHierarchyAndSnapshots) {
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    const auto scene = test::ConnectWorld(world, renderer);
    auto* actor = world.SpawnActor();
    auto* root = actor->AddComponent<SceneComponent>();
    auto* child = actor->AddSceneComponent<SceneComponent>(root, AttachmentRule::KeepLocal);
    auto* leaf = actor->AddSceneComponent<StaticMeshComponent>(child, AttachmentRule::KeepLocal);
    root->SetRelativeRotation(Eigen::Quaternionf{Eigen::AngleAxisf{0.3f, Eigen::Vector3f::UnitY()}});
    root->SetRelativeScale({-2, 3, 0.5f});
    child->SetRelativeRotation(Eigen::Quaternionf{Eigen::AngleAxisf{0.7f, Eigen::Vector3f::UnitZ()}});
    child->SetRelativeLocation({1, -2, 3});
    leaf->SetRelativeLocation({4, 5, -6});
    leaf->SetRelativeScale({1, 2, 1});
    const auto referenceLocal = [](const SceneComponent& component) -> Eigen::Matrix4f {
        return (Eigen::Translation3f{component.GetRelativeLocation()} * component.GetRelativeRotation() * Eigen::Scaling(component.GetRelativeScale())).matrix();
    };
    const auto reference = [&]() -> Eigen::Matrix4f {
        return referenceLocal(*root) * referenceLocal(*child) * referenceLocal(*leaf);
    };
    const Eigen::Matrix4f initial = reference();
    const auto snapshot = leaf->GetWorldMatrix();
    EXPECT_TRUE(snapshot.isApprox(initial, 1e-5f));
    SceneUpdateBatch batch;
    test::CollectScene(world, renderer, batch);
    for (int i = 1; i <= 4; ++i) {
        root->SetRelativeLocation({float(i), float(-i), float(2 * i)});
        if (i == 4) leaf->SetRelativeScale({0, 2, -1});
        const Eigen::Matrix4f expected = reference();
        const Eigen::Matrix4f immediate = leaf->GetWorldTransform();
        EXPECT_TRUE(immediate.isApprox(expected, 1e-5f));
        EXPECT_TRUE(leaf->GetWorldLocation().isApprox(expected.block<3, 1>(0, 3), 1e-5f));
        EXPECT_TRUE(snapshot.isApprox(initial, 1e-5f));
        test::CollectScene(world, renderer, batch);
        EXPECT_TRUE(batch.Transforms.empty());
        ASSERT_EQ(batch.LocalTransforms.size(), i == 4 ? 2u : 1u);
        EXPECT_EQ(batch.LocalTransforms[0].Id, root->GetSceneTransformId());
        const auto mesh = renderer.GetSceneRT(scene)->GetStaticMesh(leaf->GetShapeId());
        ASSERT_TRUE(mesh);
        EXPECT_TRUE(mesh->LocalToWorld.isApprox(expected, 1e-5f));
    }
}

TEST(WorldLifecycle, SceneHierarchyKeepsPendingDependenciesUntilRetirement) {
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    const auto scene = test::ConnectWorld(world, renderer);
    auto* parentActor = world.SpawnActor();
    auto* parent = parentActor->AddComponent<SceneComponent>();
    auto* child = world.SpawnActor()->AddSceneComponent<StaticMeshComponent>(parent, AttachmentRule::KeepLocal);
    SceneUpdateBatch batch;
    test::CollectScene(world, renderer, batch);
    child->SetRelativeLocation({2, 0, 0});
    parent->SetRelativeLocation({10, 0, 0});
    ASSERT_EQ(world.DestroyActor(parentActor), LifecycleRequestResult::Accepted);
    parent->SetRelativeLocation({11, 0, 0});
    world.CollectRenderUpdates();
    renderer.SealFrameGT(0);
    EXPECT_EQ(test::SceneBatch(renderer, scene, 0).LocalTransforms.size(), 2u);
    test::ConsumeFrame(renderer, 0);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(scene)->GetStaticMesh(child->GetShapeId())->LocalToWorld(0, 3), 13);
    test::CompleteFrame(renderer, 0);
    test::CollectScene(world, renderer, batch);
    EXPECT_EQ(batch.RemoveTransforms.size(), 1u);
    EXPECT_EQ(batch.TransformParents.size(), 1u);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(scene)->GetStaticMesh(child->GetShapeId())->LocalToWorld(0, 3), 2);
}

TEST(WorldLifecycle, SceneLightPoseFollowsAncestorsWithoutRecapturingLightState) {
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    const auto scene = test::ConnectWorld(world, renderer);
    auto* actor = world.SpawnActor();
    auto* parent = actor->AddComponent<SceneComponent>();
    auto* light = actor->AddSceneComponent<SpotLightComponent>(parent, AttachmentRule::KeepLocal);
    auto* survivor = actor->AddSceneComponent<SpotLightComponent>(parent, AttachmentRule::KeepLocal);
    light->SetRelativeLocation({1, 2, 3});
    survivor->SetRelativeLocation({-1, 4, 5});
    SceneUpdateBatch batch;
    test::CollectScene(world, renderer, batch);
    parent->SetRelativeLocation({10, -5, 2});
    parent->SetRelativeRotation(Eigen::Quaternionf{Eigen::AngleAxisf{.7f, Eigen::Vector3f::UnitY()}});
    parent->SetRelativeScale({-2, 3, .5f});
    test::CollectScene(world, renderer, batch);
    EXPECT_FALSE(batch.LightsChanged);
    ASSERT_EQ(batch.LocalTransforms.size(), 1u);
    EXPECT_EQ(batch.LocalTransforms[0].Id, parent->GetSceneTransformId());
    const auto actual = renderer.GetSceneRT(scene)->GetLights().GetSpotLight(light->GetLightId());
    ASSERT_TRUE(actual);
    EXPECT_TRUE(actual->Point.Position.isApprox(light->GetWorldLocation(), 1e-5f));
    EXPECT_TRUE(actual->Point.Direction.isApprox(light->GetLightDirection(), 1e-5f));
    light->SetIntensity(7);
    test::CollectScene(world, renderer, batch);
    EXPECT_TRUE(batch.LightsChanged);
    EXPECT_TRUE(batch.LocalTransforms.empty());
    const auto unchangedPose = renderer.GetSceneRT(scene)->GetLights().GetSpotLight(light->GetLightId());
    EXPECT_TRUE(unchangedPose->Point.Position.isApprox(light->GetWorldLocation(), 1e-5f));
    EXPECT_TRUE(unchangedPose->Point.Direction.isApprox(light->GetLightDirection(), 1e-5f));
    EXPECT_FLOAT_EQ(unchangedPose->Common.Intensity, 7);
    actor->RemoveComponent(light);
    test::CollectScene(world, renderer, batch);
    const auto compacted = renderer.GetSceneRT(scene)->GetLights().GetSpotLight(survivor->GetLightId());
    ASSERT_TRUE(compacted);
    EXPECT_TRUE(compacted->Point.Position.isApprox(survivor->GetWorldLocation(), 1e-5f));
    EXPECT_TRUE(compacted->Point.Direction.isApprox(survivor->GetLightDirection(), 1e-5f));
}

class TransformSubscriber final : public SceneComponent {
public:
    explicit TransformSubscriber(uint32_t& count) : _count(count) {}

protected:
    void OnTransformChanged() override { ++_count; }

private:
    uint32_t& _count;
};

TEST(WorldLifecycle, TransformSubscriptionsFollowEnableReparentAndParentRemoval) {
    uint32_t notifications = 0;
    test::ScopedWorld world;
    auto* actor = world.SpawnActor();
    auto* first = actor->AddComponent<SceneComponent>();
    auto* second = actor->AddComponent<SceneComponent>();
    auto* child = actor->AddSceneComponent<TransformSubscriber>(first, AttachmentRule::KeepLocal, notifications);
    first->SetRelativeLocation({1, 0, 0});
    world.FinalizeWorldGT();
    EXPECT_EQ(notifications, 0u);
    child->SetTransformNotificationEnabled(true);
    first->SetRelativeLocation({2, 0, 0});
    world.FinalizeWorldGT();
    EXPECT_EQ(notifications, 1u);
    child->RequestReparent(second);
    world.FinalizeWorldGT();
    EXPECT_EQ(notifications, 2u);
    first->SetRelativeLocation({3, 0, 0});
    world.FinalizeWorldGT();
    EXPECT_EQ(notifications, 2u);
    second->SetRelativeLocation({4, 0, 0});
    world.FinalizeWorldGT();
    EXPECT_EQ(notifications, 3u);
    child->SetTransformNotificationEnabled(false);
    second->SetRelativeLocation({5, 0, 0});
    world.FinalizeWorldGT();
    EXPECT_EQ(notifications, 3u);
    child->SetTransformNotificationEnabled(true);
    actor->RemoveComponent(second);
    world.FinalizeWorldGT();
    EXPECT_EQ(notifications, 4u);
    EXPECT_FALSE(child->GetAttachParent());
    child->SetRelativeLocation({9, 0, 0});
    world.FinalizeWorldGT();
    EXPECT_EQ(notifications, 5u);
}

TEST(WorldLifecycle, TransformRootsCoalesceAndCallbackMutationsNotifyNextBatch) {
    LifecycleTrace rootTrace, childTrace, otherTrace;
    test::ScopedWorld world;
    auto* actor = world.SpawnActor();
    auto* root = actor->AddComponent<LifecycleProbe>(rootTrace);
    auto* child = actor->AddSceneComponent<LifecycleProbe>(root, AttachmentRule::KeepLocal, childTrace);
    auto* other = actor->AddComponent<LifecycleProbe>(otherTrace);
    world.FinalizeWorldGT();
    rootTrace.TransformNotified = childTrace.TransformNotified = otherTrace.TransformNotified = 0;
    root->Transform = [&](auto&) {
        EXPECT_FLOAT_EQ(child->GetWorldLocation().x(), 123);
        other->SetRelativeLocation({77, 0, 0});
    };
    child->SetRelativeLocation({23, 0, 0});
    for (int i = 1; i <= 100; ++i) root->SetRelativeLocation({float(i), 0, 0});
    EXPECT_FLOAT_EQ(child->GetWorldLocation().x(), 123);
    EXPECT_EQ(rootTrace.TransformNotified, 0u);
    EXPECT_EQ(childTrace.TransformNotified, 0u);
    world.FinalizeWorldGT();
    EXPECT_EQ(rootTrace.TransformNotified, 1u);
    EXPECT_EQ(childTrace.TransformNotified, 1u);
    EXPECT_EQ(otherTrace.TransformNotified, 0u);
    EXPECT_FLOAT_EQ(other->GetWorldLocation().x(), 77);
    world.CollectRenderUpdates();
    EXPECT_EQ(otherTrace.TransformNotified, 0u);
    world.FinalizeWorldGT();
    EXPECT_EQ(otherTrace.TransformNotified, 1u);
    world.FinalizeWorldGT();
    EXPECT_EQ(otherTrace.TransformNotified, 1u);
}

TEST(WorldLifecycle, RegistrationOrderCannotHideAncestorTransformChanges) {
    LifecycleTrace trace;
    test::ScopedWorld world;
    auto draft = make_unique<Actor>();
    auto* child = draft->AddComponent<LifecycleProbe>(trace);
    auto* parent = draft->AddComponent<LifecycleProbe>(trace);
    child->AttachTo(parent);
    EXPECT_FLOAT_EQ(child->GetWorldLocation().x(), 0);
    child->Register = [&](auto&) {
        EXPECT_FLOAT_EQ(child->GetWorldLocation().x(), 0);
        parent->SetRelativeLocation({9, 0, 0});
        EXPECT_FLOAT_EQ(child->GetWorldLocation().x(), 9);
    };
    world.SpawnActor(std::move(draft));
    world.FinalizeWorldGT();
    EXPECT_FLOAT_EQ(child->GetWorldLocation().x(), 9);
}

TEST(WorldLifecycle, QueriedDraftHierarchyRevalidatesWhenJoiningWorld) {
    LifecycleTrace trace;
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    const auto sceneId = test::ConnectWorld(world, renderer);
    auto draft = make_unique<Actor>();
    auto* child = draft->AddComponent<LifecycleProbe>(trace);
    auto* parent = draft->AddComponent<SceneComponent>();
    child->AttachTo(parent);
    child->SetRelativeLocation({3, 0, 0});
    EXPECT_FLOAT_EQ(child->GetWorldLocation().x(), 3);
    parent->SetRelativeLocation({9, 0, 0});
    EXPECT_FLOAT_EQ(parent->GetWorldLocation().x(), 9);
    world.SpawnActor(std::move(draft));
    EXPECT_FLOAT_EQ(child->GetWorldLocation().x(), 12);
    SceneUpdateBatch batch;
    test::CollectScene(world, renderer, batch);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(sceneId)->GetStaticMesh(child->GetShapeId())->LocalToWorld(0, 3), 12);
}

TEST(WorldLifecycle, CallbackTransformChangesRenderNowWithoutRepeatingTheNextFrameDelta) {
    LifecycleTrace triggerTrace, targetTrace;
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    const auto sceneId = test::ConnectWorld(world, renderer);
    auto* actor = world.SpawnActor();
    auto* trigger = actor->AddComponent<LifecycleProbe>(triggerTrace);
    auto* target = actor->AddComponent<LifecycleProbe>(targetTrace);
    SceneUpdateBatch batch;
    test::CollectScene(world, renderer, batch);
    trigger->Transform = [&](auto&) { target->SetRelativeLocation({31, 0, 0}); };
    trigger->SetRelativeLocation({7, 0, 0});
    test::CollectScene(world, renderer, batch);
    EXPECT_EQ(batch.Transforms.size(), 2u);
    EXPECT_EQ(targetTrace.TransformNotified, 0u);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(sceneId)->GetStaticMesh(target->GetShapeId())->LocalToWorld(0, 3), 31);
    test::CollectScene(world, renderer, batch);
    EXPECT_EQ(targetTrace.TransformNotified, 1u);
    EXPECT_TRUE(batch.Empty());
}

TEST(WorldLifecycle, RemovingQueuedTransformsAndReusingComponentSlotsKeepsSurvivorsFresh) {
    LifecycleTrace deadTrace, liveTrace;
    test::ScopedWorld world;
    auto* actor = world.SpawnActor();
    auto* dead = actor->AddComponent<LifecycleProbe>(deadTrace);
    auto* live = actor->AddComponent<LifecycleProbe>(liveTrace);
    dead->SetRelativeLocation({5, 0, 0});
    live->SetRelativeLocation({6, 0, 0});
    actor->RemoveComponent(dead);
    world.FinalizeWorldGT();
    EXPECT_EQ(deadTrace.TransformNotified, 0u);
    EXPECT_EQ(deadTrace.Freed, 1u);
    EXPECT_EQ(liveTrace.TransformNotified, 1u);
    auto* replacement = actor->AddComponent<LifecycleProbe>(liveTrace);
    replacement->SetRelativeLocation({8, 0, 0});
    live->SetRelativeLocation({7, 0, 0});
    world.FinalizeWorldGT();
    EXPECT_EQ(liveTrace.TransformNotified, 3u);
    EXPECT_FLOAT_EQ(live->GetWorldLocation().x(), 7);
    EXPECT_FLOAT_EQ(replacement->GetWorldLocation().x(), 8);
}

TEST(WorldLifecycle, OverlappingTransformWritesMergeStateAndCaptureEachSourceOnce) {
    for (bool parentFirst : {false, true}) {
        SCOPED_TRACE(parentFirst);
        LifecycleTrace rootTrace, childTrace, leafTrace;
        Application app;
        RenderSystem renderer{&app, 1};
        test::ScopedWorld world;
        const auto scene = test::ConnectWorld(world, renderer);
        auto* actor = world.SpawnActor();
        auto* root = actor->AddComponent<LifecycleProbe>(rootTrace);
        auto* child = actor->AddSceneComponent<LifecycleProbe>(root, AttachmentRule::KeepLocal, childTrace);
        auto* leaf = actor->AddSceneComponent<LifecycleProbe>(child, AttachmentRule::KeepLocal, leafTrace);
        SceneUpdateBatch batch;
        test::CollectScene(world, renderer, batch);
        rootTrace = childTrace = leafTrace = {};
        if (parentFirst) root->SetRelativeLocation({10, 0, 0});
        leaf->SetRelativeLocation({3, 0, 0});
        child->SetRelativeLocation({2, 0, 0});
        if (!parentFirst) root->SetRelativeLocation({10, 0, 0});
        for (int i = 1; i <= 100; ++i) leaf->SetRelativeLocation({float(i), 0, 0});
        child->MarkRenderStateDirty();
        leaf->MarkRenderDynamicDataDirty();
        EXPECT_FLOAT_EQ(leaf->GetWorldLocation().x(), 112);
        EXPECT_EQ(leafTrace.TransformNotified, 0u);
        test::CollectScene(world, renderer, batch);
        EXPECT_EQ(batch.Transforms.size(), 2u);
        EXPECT_EQ(batch.MeshStates.size(), 1u);
        for (const auto* trace : {&rootTrace, &childTrace, &leafTrace}) {
            EXPECT_EQ(trace->TransformNotified, 1u);
            EXPECT_EQ(trace->Gathered, 1u);
            EXPECT_TRUE(trace->LastDirty.HasFlag(RenderDirtyFlag::Transform));
        }
        EXPECT_TRUE(childTrace.LastDirty.HasFlag(RenderDirtyFlag::State));
        EXPECT_TRUE(leafTrace.LastDirty.HasFlag(RenderDirtyFlag::DynamicData));
        EXPECT_FLOAT_EQ(renderer.GetSceneRT(scene)->GetStaticMesh(leaf->GetShapeId())->LocalToWorld(0, 3), 112);
        test::CollectScene(world, renderer, batch);
        EXPECT_TRUE(batch.Empty());
        EXPECT_EQ(leafTrace.Gathered, 1u);
    }
}

TEST(WorldLifecycle, CollectionAndNotificationKeepIndependentTransformBatches) {
    LifecycleTrace trace;
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    const auto scene = test::ConnectWorld(world, renderer);
    auto* node = world.SpawnActor()->AddComponent<LifecycleProbe>(trace);
    SceneUpdateBatch batch;
    test::CollectScene(world, renderer, batch);
    trace = {};
    const auto collectOnly = [&] {
        world.CollectRenderUpdates();
        renderer.SealFrameGT(0);
        batch = test::SceneBatch(renderer, scene, 0);
        test::ConsumeFrame(renderer, 0);
        test::CompleteFrame(renderer, 0);
    };
    for (int i = 1; i <= 3; ++i) {
        node->SetRelativeLocation({float(i), 0, 0});
        collectOnly();
        ASSERT_EQ(batch.Transforms.size(), 1u);
        EXPECT_EQ(trace.TransformNotified, 0u);
        EXPECT_FLOAT_EQ(renderer.GetSceneRT(scene)->GetStaticMesh(node->GetShapeId())->LocalToWorld(0, 3), float(i));
    }
    world.FinalizeWorldGT();
    EXPECT_EQ(trace.TransformNotified, 1u);
    collectOnly();
    EXPECT_TRUE(batch.Empty());
    for (int i = 4; i <= 6; ++i) {
        node->SetRelativeLocation({float(i), 0, 0});
        world.FinalizeWorldGT();
    }
    EXPECT_EQ(trace.TransformNotified, 4u);
    EXPECT_EQ(trace.Gathered, 3u);
    collectOnly();
    ASSERT_EQ(batch.Transforms.size(), 1u);
    EXPECT_EQ(trace.Gathered, 4u);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(scene)->GetStaticMesh(node->GetShapeId())->LocalToWorld(0, 3), 6);
}

TEST(WorldLifecycle, ReparentingCoveredSubtreesPreservesBothDirtyBranches) {
    for (bool dirtyDestination : {false, true}) {
        SCOPED_TRACE(dirtyDestination);
        LifecycleTrace firstTrace, secondTrace, childTrace, leafTrace;
        Application app;
        RenderSystem renderer{&app, 1};
        test::ScopedWorld world;
        const auto scene = test::ConnectWorld(world, renderer);
        auto* actor = world.SpawnActor();
        auto* first = actor->AddComponent<LifecycleProbe>(firstTrace);
        auto* second = actor->AddComponent<LifecycleProbe>(secondTrace);
        auto* child = actor->AddSceneComponent<LifecycleProbe>(first, AttachmentRule::KeepLocal, childTrace);
        auto* leaf = actor->AddSceneComponent<LifecycleProbe>(child, AttachmentRule::KeepLocal, leafTrace);
        SceneUpdateBatch batch;
        test::CollectScene(world, renderer, batch);
        firstTrace = secondTrace = childTrace = leafTrace = {};
        leaf->SetRelativeLocation({3, 0, 0});
        first->SetRelativeLocation({10, 0, 0});
        child->SetRelativeLocation({2, 0, 0});
        if (dirtyDestination) second->SetRelativeLocation({20, 0, 0});
        ASSERT_EQ(child->RequestReparent(second), LifecycleRequestResult::Accepted);
        test::CollectScene(world, renderer, batch);
        EXPECT_EQ(batch.Transforms.size(), dirtyDestination ? 4u : 3u);
        EXPECT_EQ(firstTrace.Gathered, 1u);
        EXPECT_EQ(secondTrace.Gathered, dirtyDestination ? 1u : 0u);
        EXPECT_EQ(childTrace.Gathered, 1u);
        EXPECT_EQ(leafTrace.Gathered, 1u);
        EXPECT_EQ(childTrace.TransformNotified, 1u);
        EXPECT_EQ(leafTrace.TransformNotified, 1u);
        EXPECT_FLOAT_EQ(renderer.GetSceneRT(scene)->GetStaticMesh(leaf->GetShapeId())->LocalToWorld(0, 3), dirtyDestination ? 25 : 5);
    }
}

TEST(WorldLifecycle, PendingTransformRootKeepsExplicitLiveDescendantWrites) {
    LifecycleTrace rootTrace, childTrace, untouchedTrace;
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    const auto scene = test::ConnectWorld(world, renderer);
    auto* parentActor = world.SpawnActor();
    auto* root = parentActor->AddComponent<LifecycleProbe>(rootTrace);
    auto* child = world.SpawnActor()->AddSceneComponent<LifecycleProbe>(root, AttachmentRule::KeepLocal, childTrace);
    auto* untouched = world.SpawnActor()->AddSceneComponent<LifecycleProbe>(root, AttachmentRule::KeepLocal, untouchedTrace);
    SceneUpdateBatch batch;
    test::CollectScene(world, renderer, batch);
    rootTrace = childTrace = untouchedTrace = {};
    child->SetRelativeLocation({2, 0, 0});
    root->SetRelativeLocation({10, 0, 0});
    ASSERT_EQ(world.DestroyActor(parentActor), LifecycleRequestResult::Accepted);
    world.CollectRenderUpdates();
    renderer.SealFrameGT(0);
    EXPECT_EQ(test::SceneBatch(renderer, scene, 0).Transforms.size(), 1u);
    test::ConsumeFrame(renderer, 0);
    test::CompleteFrame(renderer, 0);
    EXPECT_EQ(rootTrace.Gathered, 0u);
    EXPECT_EQ(childTrace.Gathered, 1u);
    EXPECT_EQ(untouchedTrace.Gathered, 0u);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(scene)->GetStaticMesh(child->GetShapeId())->LocalToWorld(0, 3), 12);
    test::CollectScene(world, renderer, batch);
    EXPECT_EQ(batch.RemoveShapes.size(), 1u);
    EXPECT_EQ(batch.Transforms.size(), 2u);
    EXPECT_FLOAT_EQ(child->GetWorldLocation().x(), 2);
    EXPECT_FLOAT_EQ(untouched->GetWorldLocation().x(), 0);
}

TEST(WorldLifecycle, PendingAncestorKeepsOverlappingLiveDescendantsCoalesced) {
    LifecycleTrace rootTrace, childTrace, leafTrace, triggerTrace;
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    const auto scene = test::ConnectWorld(world, renderer);
    auto* parentActor = world.SpawnActor();
    auto* root = parentActor->AddComponent<LifecycleProbe>(rootTrace);
    auto* child = world.SpawnActor()->AddSceneComponent<LifecycleProbe>(root, AttachmentRule::KeepLocal, childTrace);
    auto* leaf = world.SpawnActor()->AddSceneComponent<LifecycleProbe>(child, AttachmentRule::KeepLocal, leafTrace);
    auto* triggerActor = world.SpawnActor();
    auto* trigger = triggerActor->AddComponent<LifecycleProbe>(triggerTrace);
    SceneUpdateBatch batch;
    test::CollectScene(world, renderer, batch);
    rootTrace = childTrace = leafTrace = {};
    child->SetRelativeLocation({2, 0, 0});
    root->SetRelativeLocation({10, 0, 0});
    trigger->Unregister = [&](auto&) {
        EXPECT_EQ(world.DestroyActor(parentActor), LifecycleRequestResult::Accepted);
        leaf->SetRelativeLocation({3, 0, 0});
    };
    ASSERT_EQ(world.DestroyActor(triggerActor), LifecycleRequestResult::Accepted);
    test::CollectScene(world, renderer, batch);
    EXPECT_EQ(rootTrace.TransformNotified, 0u);
    EXPECT_EQ(childTrace.TransformNotified, 1u);
    EXPECT_EQ(leafTrace.TransformNotified, 1u);
    EXPECT_EQ(rootTrace.Gathered, 0u);
    EXPECT_EQ(childTrace.Gathered, 1u);
    EXPECT_EQ(leafTrace.Gathered, 1u);
    EXPECT_EQ(batch.Transforms.size(), 2u);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(scene)->GetStaticMesh(leaf->GetShapeId())->LocalToWorld(0, 3), 15);
}

TEST(WorldLifecycle, RandomizedOverlappingBranchesCaptureExactlyTheirFinalValues) {
    vector<LifecycleTrace> traces(128);
    vector<LifecycleProbe*> nodes;
    vector<float> positions(traces.size(), 0);
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    const auto scene = test::ConnectWorld(world, renderer);
    auto* actor = world.SpawnActor();
    for (size_t i = 0; i < traces.size(); ++i) {
        nodes.push_back(actor->AddSceneComponent<LifecycleProbe>(i ? Nullable<SceneComponent*>{nodes[(i - 1) / 3]} : nullptr, AttachmentRule::KeepLocal, traces[i]));
    }
    SceneUpdateBatch batch;
    test::CollectScene(world, renderer, batch);
    std::mt19937 random{0xD17A5u};
    for (int frame = 1; frame <= 120; ++frame) {
        SCOPED_TRACE(frame);
        vector<bool> changed(nodes.size(), false), state(nodes.size(), false);
        for (auto& trace : traces) trace = {};
        for (int write = 0; write < 48; ++write) {
            const size_t index = random() % nodes.size();
            positions[index] += 1;
            nodes[index]->SetRelativeLocation({positions[index], 0, 0});
            changed[index] = true;
            if (write % 7 == 0) {
                nodes[index]->MarkRenderStateDirty();
                state[index] = true;
            }
        }
        vector<float> expected = positions;
        for (size_t i = 1; i < nodes.size(); ++i) {
            expected[i] += expected[(i - 1) / 3];
            changed[i] = changed[i] || changed[(i - 1) / 3];
            EXPECT_FLOAT_EQ(nodes[i]->GetWorldLocation().x(), expected[i]);
        }
        test::CollectScene(world, renderer, batch);
        for (size_t i = 0; i < nodes.size(); ++i) {
            SCOPED_TRACE(i);
            EXPECT_EQ(traces[i].Gathered, uint32_t(changed[i] || state[i]));
            EXPECT_EQ(traces[i].TransformNotified, uint32_t(changed[i]));
            EXPECT_FLOAT_EQ(renderer.GetSceneRT(scene)->GetStaticMesh(nodes[i]->GetShapeId())->LocalToWorld(0, 3), expected[i]);
        }
    }
}

TEST(WorldLifecycle, CallbackOverlapCapturesFinalValuesOnceAndNotifiesNextBatch) {
    for (bool changeAncestor : {false, true}) {
        SCOPED_TRACE(changeAncestor);
        LifecycleTrace rootTrace, childTrace;
        Application app;
        RenderSystem renderer{&app, 1};
        test::ScopedWorld world;
        const auto scene = test::ConnectWorld(world, renderer);
        auto* actor = world.SpawnActor();
        auto* root = actor->AddComponent<LifecycleProbe>(rootTrace);
        auto* child = actor->AddSceneComponent<LifecycleProbe>(root, AttachmentRule::KeepLocal, childTrace);
        SceneUpdateBatch batch;
        test::CollectScene(world, renderer, batch);
        rootTrace = childTrace = {};
        if (changeAncestor) {
            child->Transform = [&](auto&) { root->SetRelativeLocation({10, 0, 0}); };
            child->SetRelativeLocation({2, 0, 0});
        } else {
            root->Transform = [&](auto&) { child->SetRelativeLocation({2, 0, 0}); };
            root->SetRelativeLocation({10, 0, 0});
        }
        child->MarkRenderStateDirty();
        test::CollectScene(world, renderer, batch);
        EXPECT_EQ(rootTrace.Gathered, 1u);
        EXPECT_EQ(childTrace.Gathered, 1u);
        EXPECT_EQ(rootTrace.TransformNotified, changeAncestor ? 0u : 1u);
        EXPECT_EQ(childTrace.TransformNotified, 1u);
        EXPECT_EQ(batch.Transforms.size(), 1u);
        EXPECT_EQ(batch.MeshStates.size(), 1u);
        EXPECT_FLOAT_EQ(renderer.GetSceneRT(scene)->GetStaticMesh(child->GetShapeId())->LocalToWorld(0, 3), 12);
        test::CollectScene(world, renderer, batch);
        EXPECT_TRUE(batch.Empty());
        EXPECT_EQ(rootTrace.TransformNotified, 1u);
        EXPECT_EQ(childTrace.TransformNotified, 2u);
        EXPECT_EQ(rootTrace.Gathered, 1u);
        EXPECT_EQ(childTrace.Gathered, 1u);
    }
}

TEST(WorldLifecycle, RandomizedIdentityAndPendingStateMatchesReference) {
    LifecycleTrace trace;
    test::ScopedWorld world;
    std::mt19937 random{0x260920u};
    struct Expected {
        ActorId Id;
        bool Pending;
    };
    vector<Expected> objects;
    uint32_t retired = 0;
    for (uint32_t step = 0; step < 3000; ++step) {
        SCOPED_TRACE(step);
        const auto action = random() % 4;
        if (action == 0 || objects.empty())
            objects.push_back({world.SpawnActor<LifecycleActor>(trace)->GetId(), false});
        else if (action == 1) {
            auto& selected = objects[random() % objects.size()];
            EXPECT_EQ(world.DestroyActor(selected.Id), selected.Pending ? LifecycleRequestResult::AlreadyPending : LifecycleRequestResult::Accepted);
            selected.Pending = true;
        } else if (action == 2) {
            world.FinalizeWorldGT();
            std::erase_if(objects, [&](const auto& value) { if (value.Pending) ++retired; return value.Pending; });
        } else
            world.Tick(0);
        for (const auto& object : objects) EXPECT_EQ(static_cast<bool>(world.FindLive(object.Id)), !object.Pending);
        EXPECT_EQ(trace.Freed, retired);
        EXPECT_EQ(world.GetActors().size(), objects.size());
    }
}

}  // namespace
}  // namespace radray
