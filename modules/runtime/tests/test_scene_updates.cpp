#include "scene_test_support.h"
#include <gtest/gtest.h>

#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>

namespace radray {
namespace {

class LifecycleComponent final : public ActorComponent {
public:
    explicit LifecycleComponent(vector<string>& events) : _events(events) { SetTickEnabled(true); }
    void OnRegister() override {
        EXPECT_TRUE(IsRegistered() || GetRegistrationState() == ComponentRegistration::Registering);
        _events.push_back("register");
    }
    void TickComponent(float) override { _events.push_back("tick"); }
    void OnUnregister() override {
        EXPECT_FALSE(IsRegistered());
        _events.push_back("unregister");
    }

private:
    vector<string>& _events;
};

class QuietSceneComponent final : public SceneComponent {
public:
    explicit QuietSceneComponent(uint32_t& collections) : _collections(collections) {}

protected:
    void CollectRenderUpdates(SceneWriter&, RenderDirtyFlags) override { ++_collections; }

private:
    uint32_t& _collections;
};

TEST(SceneUpdates, OrdinaryComponentsKeepTheirLifecycleWithoutAutomaticRenderUpdates) {
    vector<string> events;
    uint32_t collections = 0;
    Application app;
    RenderSystem render{&app, 1};
    test::ScopedWorld world;
    test::ConnectWorld(world, render);
    auto* actor = world.SpawnActor();
    actor->AddComponent<LifecycleComponent>(events);
    auto* scene = actor->AddComponent<QuietSceneComponent>(collections);
    scene->SetRelativeLocation({1, 2, 3});
    world.Tick(0.01f);
    SceneUpdateBatch batch;
    test::CollectScene(world, render, batch);
    EXPECT_TRUE(batch.Empty());
    EXPECT_EQ(collections, 0u);
    world.DestroyActor(actor);
    test::CollectScene(world, render, batch);
    EXPECT_TRUE(batch.Empty());
    EXPECT_EQ(events, (vector<string>{"register", "tick", "unregister"}));
}

class RenderSceneComponent final : public SceneComponent {
public:
    explicit RenderSceneComponent(vector<string>& events) : _events(events) {}
    void OnRegister() override { _events.push_back("register"); }
    void OnUnregister() override {
        EXPECT_FALSE(IsRegistered());
        _events.push_back("unregister");
        MarkRenderStateDirty();
    }

protected:
    void CreateRenderState(SceneWriter&) override {
        EXPECT_TRUE(IsRegistered() || GetRegistrationState() == ComponentRegistration::Registering);
        _events.push_back("create");
        MarkRenderStateDirty();
    }
    void DestroyRenderState(SceneWriter&) override {
        EXPECT_FALSE(IsRegistered());
        _events.push_back("destroy");
    }
    void OnTransformChanged() override { MarkRenderTransformDirty(); }
    void CollectRenderUpdates(SceneWriter&, RenderDirtyFlags dirty) override {
        _events.push_back("collect");
        EXPECT_TRUE(dirty.HasFlag(RenderDirtyFlag::State));
        EXPECT_TRUE(dirty.HasFlag(RenderDirtyFlag::Transform));
        EXPECT_TRUE(dirty.HasFlag(RenderDirtyFlag::DynamicData));
        EXPECT_FLOAT_EQ(GetWorldLocation().x(), 9);
    }

private:
    vector<string>& _events;
};

TEST(SceneUpdates, SceneDerivedComponentsUseRenderLifecycleWithoutShapeIdentity) {
    vector<string> events;
    Application app;
    RenderSystem render{&app, 1};
    test::ScopedWorld world;
    test::ConnectWorld(world, render);
    auto* actor = world.SpawnActor();
    auto* component = actor->AddComponent<RenderSceneComponent>(events);
    component->SetRelativeLocation({9, 0, 0});
    component->MarkRenderDynamicDataDirty();
    SceneUpdateBatch batch;
    test::CollectScene(world, render, batch);
    EXPECT_TRUE(batch.Empty());
    component->MarkRenderStateDirty();
    actor->RemoveComponent(component);
    test::CollectScene(world, render, batch);
    EXPECT_TRUE(batch.Empty());
    EXPECT_EQ(events, (vector<string>{"create", "register", "collect", "destroy", "unregister"}));
}

struct Collection {
    ShapeId Id;
    RenderDirtyFlags Dirty;
    Eigen::Matrix4f WorldMatrix;
};

class ProbePrimitive final : public PrimitiveComponent {
public:
    explicit ProbePrimitive(vector<Collection>& collected) : _collected(collected) {}

    void OnRegister() override {
        EXPECT_TRUE(IsRegistered() || GetRegistrationState() == ComponentRegistration::Registering);
        EXPECT_TRUE(GetShapeId().IsValid());
        MarkRenderStateDirty();
    }

    void OnUnregister() override {
        EXPECT_FALSE(IsRegistered());
        EXPECT_FALSE(GetShapeId().IsValid());
        // Deliberately omit a base call and attempt to requeue the dying component.
        MarkRenderStateDirty();
        MarkRenderTransformDirty();
        MarkRenderDynamicDataDirty();
    }

protected:
    void CollectPrimitiveUpdates(SceneWriter&, RenderDirtyFlags dirty) override {
        _collected.push_back({GetShapeId(), dirty, GetWorldMatrix()});
    }

private:
    vector<Collection>& _collected;
};

TEST(SceneUpdates, RepeatedMarksCollectFinalValuesOnceAndKeepIdentity) {
    vector<Collection> collected;
    Application app;
    RenderSystem render{&app, 1};
    test::ScopedWorld world;
    test::ConnectWorld(world, render);
    auto* component = world.SpawnActor()->AddComponent<ProbePrimitive>(collected);
    const ShapeId id = component->GetShapeId();
    EXPECT_EQ(id.Generation, 0u);
    RenderScene scene;
    SceneUpdateBatch batch;
    for (int i = 0; i < 100; ++i) {
        component->SetRelativeLocation({static_cast<float>(i), 2, 3});
        component->MarkRenderStateDirty();
        component->MarkRenderDynamicDataDirty();
    }
    test::CollectScene(world, render, batch);
    ASSERT_EQ(collected.size(), 1u);
    ASSERT_EQ(batch.CreateShapes, vector<ShapeId>{id});
    EXPECT_TRUE(batch.RemoveShapes.empty());
    EXPECT_TRUE(collected.back().Dirty.HasFlag(RenderDirtyFlag::State));
    EXPECT_TRUE(collected.back().Dirty.HasFlag(RenderDirtyFlag::Transform));
    EXPECT_TRUE(collected.back().Dirty.HasFlag(RenderDirtyFlag::DynamicData));
    EXPECT_FLOAT_EQ(collected.back().WorldMatrix(0, 3), 99);
    scene.Apply(batch);
    EXPECT_TRUE(scene.ContainsShape(id));

    batch.Clear();
    test::CollectScene(world, render, batch);
    EXPECT_EQ(collected.size(), 1u);
    EXPECT_TRUE(batch.CreateShapes.empty());

    component->SetRelativeLocation({101, 0, 0});
    test::CollectScene(world, render, batch);
    ASSERT_EQ(collected.size(), 2u);
    EXPECT_EQ(collected.back().Dirty, RenderDirtyFlags{RenderDirtyFlag::Transform});
    EXPECT_FLOAT_EQ(collected.back().WorldMatrix(0, 3), 101);
    EXPECT_TRUE(batch.CreateShapes.empty());
    component->MarkRenderStateDirty();
    component->MarkRenderDynamicDataDirty();
    test::CollectScene(world, render, batch);
    ASSERT_EQ(collected.size(), 3u);
    EXPECT_EQ(collected.back().Id, id);
    EXPECT_FALSE(collected.back().Dirty.HasFlag(RenderDirtyFlag::Transform));
    EXPECT_TRUE(batch.CreateShapes.empty());
    EXPECT_TRUE(scene.ContainsShape(id));
}

TEST(SceneUpdates, RegistrationCapturesPreexistingAndLatestState) {
    vector<Collection> collected;
    Application app;
    RenderSystem render{&app, 1};
    test::ScopedWorld world;
    test::ConnectWorld(world, render);
    auto actor = make_unique<Actor>();
    auto* component = actor->AddComponent<ProbePrimitive>(collected);
    component->SetRelativeLocation({7, 0, 0});
    component->MarkRenderStateDirty();
    EXPECT_FALSE(component->GetShapeId().IsValid());
    SceneUpdateBatch batch;
    test::CollectScene(world, render, batch);
    EXPECT_TRUE(collected.empty());
    world.SpawnActor(std::move(actor));
    component->SetRelativeLocation({9, 0, 0});
    test::CollectScene(world, render, batch);
    ASSERT_EQ(collected.size(), 1u);
    ASSERT_EQ(batch.CreateShapes.size(), 1u);
    EXPECT_EQ(batch.CreateShapes[0], component->GetShapeId());
    EXPECT_FLOAT_EQ(collected[0].WorldMatrix(0, 3), 9);
}

TEST(SceneUpdates, DestroyBeforeCollectionCancelsCreateAndAllowsGenerationGap) {
    vector<Collection> collected;
    Application app;
    RenderSystem render{&app, 1};
    test::ScopedWorld world;
    test::ConnectWorld(world, render);
    Actor* actor = world.SpawnActor();
    auto* component = actor->AddComponent<ProbePrimitive>(collected);
    const ShapeId canceled = component->GetShapeId();
    actor->RemoveComponent(component);
    SceneUpdateBatch batch;
    test::CollectScene(world, render, batch);
    EXPECT_TRUE(batch.CreateShapes.empty());
    EXPECT_TRUE(batch.RemoveShapes.empty());
    EXPECT_TRUE(collected.empty());
    auto* replacement = actor->AddComponent<ProbePrimitive>(collected);
    const ShapeId id = replacement->GetShapeId();
    EXPECT_EQ(id.Index, canceled.Index);
    EXPECT_GT(id.Generation, canceled.Generation);
    test::CollectScene(world, render, batch);
    RenderScene scene;
    scene.Apply(batch);
    EXPECT_TRUE(scene.ContainsShape(id));
    EXPECT_FALSE(scene.ContainsShape(canceled));
}

TEST(SceneUpdates, RemovingQueuedEntriesRepairsSwappedIndex) {
    for (int removed = 0; removed < 3; ++removed) {
        SCOPED_TRACE(removed);
        vector<Collection> collected;
        Application app;
        RenderSystem render{&app, 1};
        test::ScopedWorld world;
        test::ConnectWorld(world, render);
        Actor* actor = world.SpawnActor();
        vector<ProbePrimitive*> components;
        for (int i = 0; i < 3; ++i) components.push_back(actor->AddComponent<ProbePrimitive>(collected));
        actor->RemoveComponent(components[removed]);
        components.erase(components.begin() + removed);
        for (auto* component : components) {
            for (int i = 0; i < 100; ++i) component->MarkRenderTransformDirty();
        }
        SceneUpdateBatch batch;
        test::CollectScene(world, render, batch);
        ASSERT_EQ(collected.size(), 2u);
        ASSERT_EQ(batch.CreateShapes.size(), 2u);
        EXPECT_NE(collected[0].Id, collected[1].Id);
        EXPECT_TRUE(batch.RemoveShapes.empty());
        batch.Clear();
        for (auto* component : components) component->MarkRenderStateDirty();
        world.DestroyActor(actor);
        test::CollectScene(world, render, batch);
        EXPECT_EQ(collected.size(), 2u);
        EXPECT_EQ(batch.RemoveShapes.size(), 2u);
        EXPECT_TRUE(batch.CreateShapes.empty());
    }
}

TEST(SceneUpdates, SealedCreateSurvivesSourceDestructionUntilOrderedRemoval) {
    vector<Collection> collected;
    Application app;
    RenderSystem render{&app, 1};
    test::ScopedWorld world;
    test::ConnectWorld(world, render);
    Actor* actor = world.SpawnActor();
    auto* component = actor->AddComponent<ProbePrimitive>(collected);
    const ShapeId id = component->GetShapeId();
    SceneUpdateBatch create;
    test::CollectScene(world, render, create);
    component->MarkRenderStateDirty();
    component->SetRelativeLocation({12, 0, 0});
    world.DestroyActor(actor);
    SceneUpdateBatch remove;
    test::CollectScene(world, render, remove);
    EXPECT_EQ(collected.size(), 1u);
    EXPECT_TRUE(remove.CreateShapes.empty());
    ASSERT_EQ(remove.RemoveShapes, vector<ShapeId>{id});
    RenderScene scene;
    scene.Apply(create);
    EXPECT_TRUE(scene.ContainsShape(id));
    scene.Apply(remove);
    EXPECT_FALSE(scene.ContainsShape(id));
}

TEST(SceneUpdates, SameBatchRemovesOldGenerationBeforeCreatingReplacement) {
    Application app;
    RenderSystem render{&app, 1};
    test::ScopedWorld world;
    test::ConnectWorld(world, render);
    auto* actor = world.SpawnActor();
    auto* first = actor->AddComponent<PrimitiveComponent>();
    const ShapeId oldId = first->GetShapeId();
    SceneUpdateBatch batch;
    test::CollectScene(world, render, batch);
    RenderScene scene;
    scene.Apply(batch);
    batch.Clear();
    actor->RemoveComponent(first);
    world.FinalizeWorldGT();
    auto* canceled = actor->AddComponent<PrimitiveComponent>();
    actor->RemoveComponent(canceled);
    world.FinalizeWorldGT();
    const ShapeId newId = actor->AddComponent<PrimitiveComponent>()->GetShapeId();
    EXPECT_EQ(newId.Index, oldId.Index);
    EXPECT_GT(newId.Generation, oldId.Generation + 1);
    test::CollectScene(world, render, batch);
    ASSERT_EQ(batch.RemoveShapes, vector<ShapeId>{oldId});
    ASSERT_EQ(batch.CreateShapes, vector<ShapeId>{newId});
    scene.Apply(batch);
    EXPECT_FALSE(scene.ContainsShape(oldId));
    EXPECT_TRUE(scene.ContainsShape(newId));
}

TEST(SceneUpdates, ParentChangesReparentAndRemovalCollectLatestChildTransform) {
    vector<Collection> collected;
    Application app;
    RenderSystem render{&app, 1};
    test::ScopedWorld world;
    test::ConnectWorld(world, render);
    auto* actor = world.SpawnActor();
    auto* parent = actor->AddComponent<SceneComponent>();
    auto* other = actor->AddComponent<SceneComponent>();
    auto* child = actor->AddComponent<ProbePrimitive>(collected);
    child->RequestReparent(parent);
    SceneUpdateBatch batch;
    test::CollectScene(world, render, batch);
    batch.Clear();
    collected.clear();
    for (int i = 0; i < 100; ++i) {
        parent->SetRelativeLocation({static_cast<float>(i), 0, 0});
        child->SetRelativeLocation({1, 0, 0});
    }
    test::CollectScene(world, render, batch);
    ASSERT_EQ(collected.size(), 1u);
    EXPECT_FLOAT_EQ(collected.back().WorldMatrix(0, 3), 100);
    EXPECT_EQ(collected.back().Dirty, RenderDirtyFlags{RenderDirtyFlag::Transform});
    other->SetRelativeLocation({200, 0, 0});
    child->RequestReparent(other);
    test::CollectScene(world, render, batch);
    ASSERT_EQ(collected.size(), 2u);
    EXPECT_FLOAT_EQ(collected.back().WorldMatrix(0, 3), 201);
    actor->RemoveComponent(other);
    test::CollectScene(world, render, batch);
    ASSERT_EQ(collected.size(), 3u);
    EXPECT_FLOAT_EQ(collected.back().WorldMatrix(0, 3), 1);
    EXPECT_FALSE(child->GetAttachParent());
}

TEST(SceneUpdates, ClearRetainsBatchCapacity) {
    SceneUpdateBatch batch;
    batch.CreateShapes.push_back({0, 1});
    batch.RemoveShapes.push_back({1, 1});
    const auto createCapacity = batch.CreateShapes.capacity();
    const auto removeCapacity = batch.RemoveShapes.capacity();
    batch.Clear();
    EXPECT_TRUE(batch.CreateShapes.empty());
    EXPECT_TRUE(batch.RemoveShapes.empty());
    EXPECT_EQ(batch.CreateShapes.capacity(), createCapacity);
    EXPECT_EQ(batch.RemoveShapes.capacity(), removeCapacity);
}

TEST(SceneUpdates, GenerationZeroWorksForNewSlotsAndEarlierSparseHoles) {
    EXPECT_FALSE(ShapeId{}.IsValid());
    EXPECT_TRUE((ShapeId{0, 0}.IsValid()));
    RenderScene scene;
    SceneUpdateBatch batch;
    batch.CreateShapes.push_back({4, 0});
    scene.Apply(batch);
    EXPECT_TRUE(scene.ContainsShape({4, 0}));
    EXPECT_FALSE(scene.ContainsShape({0, 0}));
    batch.Clear();
    batch.CreateShapes.push_back({0, 0});
    scene.Apply(batch);
    EXPECT_TRUE(scene.ContainsShape({0, 0}));
    batch.Clear();
    batch.RemoveShapes.push_back({0, 0});
    scene.Apply(batch);
    EXPECT_FALSE(scene.ContainsShape({0, 0}));
    batch.Clear();
    batch.CreateShapes.push_back({0, 1});
    scene.Apply(batch);
    EXPECT_TRUE(scene.ContainsShape({0, 1}));
    EXPECT_TRUE(scene.ContainsShape({4, 0}));
}

TEST(SceneUpdatesDeathTest, RejectsStaleIdsAndDuplicateCreation) {
    RenderScene scene;
    SceneUpdateBatch batch;
    batch.CreateShapes.push_back({0, 0});
    scene.Apply(batch);
    EXPECT_DEATH(scene.Apply(batch), "");
    batch.Clear();
    batch.RemoveShapes.push_back({0, 0});
    batch.CreateShapes.push_back({0, 1});
    scene.Apply(batch);
    batch.Clear();
    batch.RemoveShapes.push_back({0, 0});
    EXPECT_DEATH(scene.Apply(batch), "");
    EXPECT_TRUE(scene.ContainsShape({0, 1}));
    batch.RemoveShapes[0] = {0, 1};
    scene.Apply(batch);
    batch.Clear();
    batch.CreateShapes.push_back({0, 1});
    EXPECT_DEATH(scene.Apply(batch), "");
    batch.CreateShapes[0] = {0, 0};
    EXPECT_DEATH(scene.Apply(batch), "");
    batch.CreateShapes[0] = {};
    EXPECT_DEATH(scene.Apply(batch), "");
}

class MutatingPrimitive final : public PrimitiveComponent {
public:
    enum class Action { Mark,
                        Transform,
                        Remove,
                        Spawn,
                        Flush };
    explicit MutatingPrimitive(Action action) : _action(action) {}

protected:
    void CollectPrimitiveUpdates(SceneWriter&, RenderDirtyFlags) override {
        switch (_action) {
            case Action::Mark: MarkRenderStateDirty(); break;
            case Action::Transform: SetRelativeLocation({1, 0, 0}); break;
            case Action::Remove: GetOwner()->RemoveComponent(this); break;
            case Action::Spawn: GetWorld()->SpawnActor(); break;
            case Action::Flush: {
                GetWorld()->CollectRenderUpdates();
                break;
            }
        }
    }

private:
    Action _action;
};

TEST(SceneUpdatesDeathTest, CollectionRejectsMutationAndRecursion) {
    for (auto action : {MutatingPrimitive::Action::Mark, MutatingPrimitive::Action::Transform,
                        MutatingPrimitive::Action::Remove, MutatingPrimitive::Action::Spawn, MutatingPrimitive::Action::Flush}) {
        Application app;
        RenderSystem render{&app, 1};
        test::ScopedWorld world;
        test::ConnectWorld(world, render);
        world.SpawnActor()->AddComponent<MutatingPrimitive>(action);
        SceneUpdateBatch batch;
        EXPECT_DEATH(test::CollectScene(world, render, batch), "");
    }
}

TEST(SceneUpdatesDeathTest, WriterRejectsStaleIdentityAndOccupiedFlight) {
    Application app;
    RenderSystem render{&app, 1};
    const auto scene = render.CreateSceneGT();
    auto writer = render.GetSceneWriterGT(scene);
    auto id = writer->CreateShape();
    writer->RemoveShape(id);
    EXPECT_DEATH(writer->SetTransform(id, Eigen::Matrix4f::Identity()), "");
    render.SealFrameGT(0);
    EXPECT_DEATH(render.SealFrameGT(0), "");
}

}  // namespace
}  // namespace radray
