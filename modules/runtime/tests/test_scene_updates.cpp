#include <gtest/gtest.h>

#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>

namespace radray {
namespace {

class LifecycleComponent final : public ActorComponent {
public:
    explicit LifecycleComponent(vector<string>& events) : _events(events) {}
    void OnRegister() override {
        EXPECT_TRUE(IsRegistered());
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
    void CollectRenderUpdates(SceneUpdateBatch&, RenderDirtyFlags) override { ++_collections; }

private:
    uint32_t& _collections;
};

TEST(SceneUpdates, OrdinaryComponentsKeepTheirLifecycleWithoutAutomaticRenderUpdates) {
    vector<string> events;
    uint32_t collections = 0;
    World world;
    auto* actor = world.SpawnActor();
    actor->AddComponent<LifecycleComponent>(events);
    auto* scene = actor->AddComponent<QuietSceneComponent>(collections);
    scene->SetRelativeLocation({1, 2, 3});
    world.Tick(0.01f);
    SceneUpdateBatch batch;
    world.FlushRenderUpdates(batch);
    EXPECT_TRUE(batch.Empty());
    EXPECT_EQ(collections, 0u);
    world.DestroyActor(actor);
    world.FlushRenderUpdates(batch);
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
    void CreateRenderState(World&) override {
        EXPECT_TRUE(IsRegistered());
        _events.push_back("create");
        MarkRenderStateDirty();
    }
    void DestroyRenderState(World&) override {
        EXPECT_FALSE(IsRegistered());
        _events.push_back("destroy");
    }
    void OnTransformChanged() override { MarkRenderTransformDirty(); }
    void CollectRenderUpdates(SceneUpdateBatch&, RenderDirtyFlags dirty) override {
        _events.push_back("collect");
        EXPECT_TRUE(dirty.HasFlag(RenderDirtyFlag::State));
        EXPECT_TRUE(dirty.HasFlag(RenderDirtyFlag::Transform));
        EXPECT_TRUE(dirty.HasFlag(RenderDirtyFlag::DynamicData));
        EXPECT_FLOAT_EQ(GetWorldLocation().x(), 9);
    }

private:
    vector<string>& _events;
};

TEST(SceneUpdates, SceneDerivedComponentsUseRenderLifecycleWithoutPrimitiveIdentity) {
    vector<string> events;
    World world;
    auto* actor = world.SpawnActor();
    auto* component = actor->AddComponent<RenderSceneComponent>(events);
    component->SetRelativeLocation({9, 0, 0});
    component->MarkRenderDynamicDataDirty();
    SceneUpdateBatch batch;
    world.FlushRenderUpdates(batch);
    EXPECT_TRUE(batch.Empty());
    component->MarkRenderStateDirty();
    actor->RemoveComponent(component);
    world.FlushRenderUpdates(batch);
    EXPECT_TRUE(batch.Empty());
    EXPECT_EQ(events, (vector<string>{"create", "register", "collect", "destroy", "unregister"}));
}

struct Collection {
    PrimitiveId Id;
    RenderDirtyFlags Dirty;
    Eigen::Matrix4f WorldMatrix;
};

class ProbePrimitive final : public PrimitiveComponent {
public:
    explicit ProbePrimitive(vector<Collection>& collected) : _collected(collected) {}

    void OnRegister() override {
        EXPECT_TRUE(IsRegistered());
        EXPECT_TRUE(GetPrimitiveId().IsValid());
        MarkRenderStateDirty();
    }

    void OnUnregister() override {
        EXPECT_FALSE(IsRegistered());
        EXPECT_FALSE(GetPrimitiveId().IsValid());
        // Deliberately omit a base call and attempt to requeue the dying component.
        MarkRenderStateDirty();
        MarkRenderTransformDirty();
        MarkRenderDynamicDataDirty();
    }

protected:
    void CollectPrimitiveUpdates(SceneUpdateBatch&, RenderDirtyFlags dirty) override {
        _collected.push_back({GetPrimitiveId(), dirty, GetWorldMatrix()});
    }

private:
    vector<Collection>& _collected;
};

TEST(SceneUpdates, RepeatedMarksCollectFinalValuesOnceAndKeepIdentity) {
    vector<Collection> collected;
    World world;
    auto* component = world.SpawnActor()->AddComponent<ProbePrimitive>(collected);
    const PrimitiveId id = component->GetPrimitiveId();
    Scene scene;
    SceneUpdateBatch batch;
    for (int i = 0; i < 100; ++i) {
        component->SetRelativeLocation({static_cast<float>(i), 2, 3});
        component->MarkRenderStateDirty();
        component->MarkRenderDynamicDataDirty();
    }
    world.FlushRenderUpdates(batch);
    ASSERT_EQ(collected.size(), 1u);
    ASSERT_EQ(batch.CreatePrimitives, vector<PrimitiveId>{id});
    EXPECT_TRUE(batch.RemovePrimitives.empty());
    EXPECT_TRUE(collected.back().Dirty.HasFlag(RenderDirtyFlag::State));
    EXPECT_TRUE(collected.back().Dirty.HasFlag(RenderDirtyFlag::Transform));
    EXPECT_TRUE(collected.back().Dirty.HasFlag(RenderDirtyFlag::DynamicData));
    EXPECT_FLOAT_EQ(collected.back().WorldMatrix(0, 3), 99);
    scene.Apply(batch);
    EXPECT_TRUE(scene.ContainsPrimitive(id));

    batch.Clear();
    world.FlushRenderUpdates(batch);
    EXPECT_EQ(collected.size(), 1u);
    EXPECT_TRUE(batch.CreatePrimitives.empty());

    component->SetRelativeLocation({101, 0, 0});
    world.FlushRenderUpdates(batch);
    ASSERT_EQ(collected.size(), 2u);
    EXPECT_EQ(collected.back().Dirty, RenderDirtyFlags{RenderDirtyFlag::Transform});
    EXPECT_FLOAT_EQ(collected.back().WorldMatrix(0, 3), 101);
    EXPECT_TRUE(batch.CreatePrimitives.empty());
    component->MarkRenderStateDirty();
    component->MarkRenderDynamicDataDirty();
    world.FlushRenderUpdates(batch);
    ASSERT_EQ(collected.size(), 3u);
    EXPECT_EQ(collected.back().Id, id);
    EXPECT_FALSE(collected.back().Dirty.HasFlag(RenderDirtyFlag::Transform));
    EXPECT_TRUE(batch.CreatePrimitives.empty());
    EXPECT_TRUE(scene.ContainsPrimitive(id));
}

TEST(SceneUpdates, RegistrationCapturesPreexistingAndLatestState) {
    vector<Collection> collected;
    World world;
    auto actor = make_unique<Actor>();
    auto* component = actor->AddComponent<ProbePrimitive>(collected);
    component->SetRelativeLocation({7, 0, 0});
    component->MarkRenderStateDirty();
    EXPECT_FALSE(component->GetPrimitiveId().IsValid());
    SceneUpdateBatch batch;
    world.FlushRenderUpdates(batch);
    EXPECT_TRUE(collected.empty());
    world.SpawnActor(std::move(actor));
    component->SetRelativeLocation({9, 0, 0});
    world.FlushRenderUpdates(batch);
    ASSERT_EQ(collected.size(), 1u);
    ASSERT_EQ(batch.CreatePrimitives.size(), 1u);
    EXPECT_EQ(batch.CreatePrimitives[0], component->GetPrimitiveId());
    EXPECT_FLOAT_EQ(collected[0].WorldMatrix(0, 3), 9);
}

TEST(SceneUpdates, DestroyBeforeCollectionCancelsCreateAndAllowsGenerationGap) {
    vector<Collection> collected;
    World world;
    Actor* actor = world.SpawnActor();
    auto* component = actor->AddComponent<ProbePrimitive>(collected);
    const PrimitiveId canceled = component->GetPrimitiveId();
    actor->RemoveComponent(component);
    SceneUpdateBatch batch;
    world.FlushRenderUpdates(batch);
    EXPECT_TRUE(batch.CreatePrimitives.empty());
    EXPECT_TRUE(batch.RemovePrimitives.empty());
    EXPECT_TRUE(collected.empty());
    auto* replacement = actor->AddComponent<ProbePrimitive>(collected);
    const PrimitiveId id = replacement->GetPrimitiveId();
    EXPECT_EQ(id.Index, canceled.Index);
    EXPECT_GT(id.Generation, canceled.Generation);
    world.FlushRenderUpdates(batch);
    Scene scene;
    scene.Apply(batch);
    EXPECT_TRUE(scene.ContainsPrimitive(id));
    EXPECT_FALSE(scene.ContainsPrimitive(canceled));
}

TEST(SceneUpdates, RemovingQueuedEntriesRepairsSwappedIndex) {
    for (int removed = 0; removed < 3; ++removed) {
        SCOPED_TRACE(removed);
        vector<Collection> collected;
        World world;
        Actor* actor = world.SpawnActor();
        vector<ProbePrimitive*> components;
        for (int i = 0; i < 3; ++i) components.push_back(actor->AddComponent<ProbePrimitive>(collected));
        actor->RemoveComponent(components[removed]);
        components.erase(components.begin() + removed);
        for (auto* component : components) {
            for (int i = 0; i < 100; ++i) component->MarkRenderTransformDirty();
        }
        SceneUpdateBatch batch;
        world.FlushRenderUpdates(batch);
        ASSERT_EQ(collected.size(), 2u);
        ASSERT_EQ(batch.CreatePrimitives.size(), 2u);
        EXPECT_NE(collected[0].Id, collected[1].Id);
        EXPECT_TRUE(batch.RemovePrimitives.empty());
        batch.Clear();
        for (auto* component : components) component->MarkRenderStateDirty();
        world.DestroyActor(actor);
        world.FlushRenderUpdates(batch);
        EXPECT_EQ(collected.size(), 2u);
        EXPECT_EQ(batch.RemovePrimitives.size(), 2u);
        EXPECT_TRUE(batch.CreatePrimitives.empty());
    }
}

TEST(SceneUpdates, SealedCreateSurvivesSourceDestructionUntilOrderedRemoval) {
    vector<Collection> collected;
    World world;
    Actor* actor = world.SpawnActor();
    auto* component = actor->AddComponent<ProbePrimitive>(collected);
    const PrimitiveId id = component->GetPrimitiveId();
    SceneUpdateBatch create;
    world.FlushRenderUpdates(create);
    component->MarkRenderStateDirty();
    component->SetRelativeLocation({12, 0, 0});
    world.DestroyActor(actor);
    SceneUpdateBatch remove;
    world.FlushRenderUpdates(remove);
    EXPECT_EQ(collected.size(), 1u);
    EXPECT_TRUE(remove.CreatePrimitives.empty());
    ASSERT_EQ(remove.RemovePrimitives, vector<PrimitiveId>{id});
    Scene scene;
    scene.Apply(create);
    EXPECT_TRUE(scene.ContainsPrimitive(id));
    scene.Apply(remove);
    EXPECT_FALSE(scene.ContainsPrimitive(id));
}

TEST(SceneUpdates, SameBatchRemovesOldGenerationBeforeCreatingReplacement) {
    World world;
    auto* actor = world.SpawnActor();
    auto* first = actor->AddComponent<PrimitiveComponent>();
    const PrimitiveId oldId = first->GetPrimitiveId();
    SceneUpdateBatch batch;
    world.FlushRenderUpdates(batch);
    Scene scene;
    scene.Apply(batch);
    batch.Clear();
    actor->RemoveComponent(first);
    auto* canceled = actor->AddComponent<PrimitiveComponent>();
    actor->RemoveComponent(canceled);
    const PrimitiveId newId = actor->AddComponent<PrimitiveComponent>()->GetPrimitiveId();
    EXPECT_EQ(newId.Index, oldId.Index);
    EXPECT_GT(newId.Generation, oldId.Generation + 1);
    world.FlushRenderUpdates(batch);
    ASSERT_EQ(batch.RemovePrimitives, vector<PrimitiveId>{oldId});
    ASSERT_EQ(batch.CreatePrimitives, vector<PrimitiveId>{newId});
    scene.Apply(batch);
    EXPECT_FALSE(scene.ContainsPrimitive(oldId));
    EXPECT_TRUE(scene.ContainsPrimitive(newId));
}

TEST(SceneUpdates, ParentChangesReparentAndRemovalCollectLatestChildTransform) {
    vector<Collection> collected;
    World world;
    auto* actor = world.SpawnActor();
    auto* parent = actor->AddComponent<SceneComponent>();
    auto* other = actor->AddComponent<SceneComponent>();
    auto* child = actor->AddComponent<ProbePrimitive>(collected);
    child->AttachTo(parent);
    SceneUpdateBatch batch;
    world.FlushRenderUpdates(batch);
    batch.Clear();
    collected.clear();
    for (int i = 0; i < 100; ++i) {
        parent->SetRelativeLocation({static_cast<float>(i), 0, 0});
        child->SetRelativeLocation({1, 0, 0});
    }
    world.FlushRenderUpdates(batch);
    ASSERT_EQ(collected.size(), 1u);
    EXPECT_FLOAT_EQ(collected.back().WorldMatrix(0, 3), 100);
    EXPECT_EQ(collected.back().Dirty, RenderDirtyFlags{RenderDirtyFlag::Transform});
    other->SetRelativeLocation({200, 0, 0});
    child->AttachTo(other);
    world.FlushRenderUpdates(batch);
    ASSERT_EQ(collected.size(), 2u);
    EXPECT_FLOAT_EQ(collected.back().WorldMatrix(0, 3), 201);
    actor->RemoveComponent(other);
    world.FlushRenderUpdates(batch);
    ASSERT_EQ(collected.size(), 3u);
    EXPECT_FLOAT_EQ(collected.back().WorldMatrix(0, 3), 1);
    EXPECT_FALSE(child->GetAttachParent());
}

TEST(SceneUpdates, ClearRetainsBatchCapacity) {
    SceneUpdateBatch batch;
    batch.CreatePrimitives.push_back({0, 1});
    batch.RemovePrimitives.push_back({1, 1});
    const auto createCapacity = batch.CreatePrimitives.capacity();
    const auto removeCapacity = batch.RemovePrimitives.capacity();
    batch.Clear();
    EXPECT_TRUE(batch.CreatePrimitives.empty());
    EXPECT_TRUE(batch.RemovePrimitives.empty());
    EXPECT_EQ(batch.CreatePrimitives.capacity(), createCapacity);
    EXPECT_EQ(batch.RemovePrimitives.capacity(), removeCapacity);
}

TEST(SceneUpdatesDeathTest, RejectsStaleIdsAndDuplicateCreation) {
    Scene scene;
    SceneUpdateBatch batch;
    batch.CreatePrimitives.push_back({0, 1});
    scene.Apply(batch);
    EXPECT_DEATH(scene.Apply(batch), "");
    batch.Clear();
    batch.RemovePrimitives.push_back({0, 1});
    batch.CreatePrimitives.push_back({0, 2});
    scene.Apply(batch);
    batch.Clear();
    batch.RemovePrimitives.push_back({0, 1});
    EXPECT_DEATH(scene.Apply(batch), "");
    EXPECT_TRUE(scene.ContainsPrimitive({0, 2}));
    batch.RemovePrimitives[0] = {0, 2};
    scene.Apply(batch);
    batch.Clear();
    batch.CreatePrimitives.push_back({0, 1});
    EXPECT_DEATH(scene.Apply(batch), "");
    batch.CreatePrimitives[0] = {};
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
    void CollectPrimitiveUpdates(SceneUpdateBatch&, RenderDirtyFlags) override {
        switch (_action) {
            case Action::Mark: MarkRenderStateDirty(); break;
            case Action::Transform: SetRelativeLocation({1, 0, 0}); break;
            case Action::Remove: GetOwner()->RemoveComponent(this); break;
            case Action::Spawn: GetWorld()->SpawnActor(); break;
            case Action::Flush: {
                SceneUpdateBatch nested;
                GetWorld()->FlushRenderUpdates(nested);
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
        World world;
        world.SpawnActor()->AddComponent<MutatingPrimitive>(action);
        SceneUpdateBatch batch;
        EXPECT_DEATH(world.FlushRenderUpdates(batch), "");
    }
}

TEST(SceneUpdatesDeathTest, CollectionRequiresEmptyBatch) {
    World world;
    world.SpawnActor()->AddComponent<PrimitiveComponent>();
    SceneUpdateBatch batch;
    world.FlushRenderUpdates(batch);
    EXPECT_DEATH(world.FlushRenderUpdates(batch), "");
}

}  // namespace
}  // namespace radray
