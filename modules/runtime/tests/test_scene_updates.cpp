#include "scene_test_support.h"
#include <gtest/gtest.h>

#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>

namespace radray {
namespace {

TEST(SceneUpdates, PackedTransformsSurviveGrowthAndBulkChangesPublishFinalValues) {
    Application app;
    RenderSystem render{&app, 2};
    test::ScopedWorld world;
    const auto scene = test::ConnectWorld(world, render);
    auto* actor = world.SpawnActor();
    vector<SceneComponent*> components;
    vector<WorldTransformUpdate> updates;
    for (uint32_t i = 0; i < 257; ++i) {
        auto* component = actor->AddComponent<SceneComponent>();
        component->SetRelativeLocation({float(i), 1, 2});
        components.push_back(component);
        updates.push_back({component->GetWorldTransformId(), {{float(i + 10), 3, 4}, Eigen::Quaternionf::Identity(), Eigen::Vector3f::Ones()}});
    }
    for (uint32_t i = 0; i < components.size(); ++i)
        EXPECT_FLOAT_EQ(components[i]->GetWorldLocation().x(), float(i));
    SceneUpdateBatch batch;
    test::CollectScene(world, render, batch);
    ASSERT_EQ(batch.CreateTransforms.size(), components.size());
    updates[128].Local = {{2, 3, 4}, Eigen::Quaternionf{Eigen::AngleAxisf{0.7f, Eigen::Vector3f::UnitY()}}, {-2, 3, 0.5f}};
    ASSERT_TRUE(world.SetLocalTransforms(updates));
    const Eigen::Matrix4f expected = (Eigen::Translation3f{2, 3, 4} * Eigen::AngleAxisf{0.7f, Eigen::Vector3f::UnitY()} * Eigen::Scaling(-2.0f, 3.0f, 0.5f)).matrix();
    EXPECT_TRUE(components[128]->GetWorldMatrix().isApprox(expected, 1e-5f));
    components[127]->SetRelativeTransform(updates[128].Local);
    EXPECT_TRUE(components[127]->GetWorldMatrix().isApprox(expected, 1e-5f));
    components[64]->SetRelativeLocation({999, 3, 4});
    EXPECT_FLOAT_EQ(components[64]->GetWorldLocation().x(), 999);
    world.CollectRenderUpdates();
    components[64]->SetRelativeLocation({1000, 3, 4});
    world.CollectRenderUpdates();
    render.SealFrameGT(0);
    const auto& sealed = test::SceneBatch(render, scene, 0);
    ASSERT_EQ(sealed.LocalTransforms.size(), components.size());
    for (const auto& update : sealed.LocalTransforms) {
        auto it = std::find_if(components.begin(), components.end(), [&](auto* value) { return value->GetSceneTransformId() == update.Id; });
        ASSERT_NE(it, components.end());
        EXPECT_TRUE(update.Local.ToMatrix().isApprox((*it)->GetWorldMatrix(), 1e-5f));
    }
    components[64]->SetRelativeLocation({2000, 3, 4});
    const auto frozen = std::find_if(sealed.LocalTransforms.begin(), sealed.LocalTransforms.end(), [&](const auto& value) { return value.Id == components[64]->GetSceneTransformId(); });
    ASSERT_NE(frozen, sealed.LocalTransforms.end());
    EXPECT_FLOAT_EQ(frozen->Local.Translation[0], 1000);
    test::ConsumeFrame(render, 0);
    test::CompleteFrame(render, 0);
    test::CollectScene(world, render, batch);
    ASSERT_EQ(batch.LocalTransforms.size(), 1u);
    EXPECT_FLOAT_EQ(batch.LocalTransforms[0].Local.Translation[0], 2000);
    test::CollectScene(world, render, batch);
    EXPECT_TRUE(batch.Empty());
}

TEST(SceneUpdates, BulkTransformHandlesRejectRetiredGenerationsAndSurviveReconnect) {
    Application app;
    RenderSystem render{&app, 1};
    test::ScopedWorld world;
    test::ConnectWorld(world, render);
    auto* actor = world.SpawnActor();
    auto* removed = actor->AddComponent<SceneComponent>();
    const auto stale = removed->GetWorldTransformId();
    auto* survivor = actor->AddComponent<SceneComponent>();
    const auto stable = survivor->GetWorldTransformId();
    SceneUpdateBatch batch;
    test::CollectScene(world, render, batch);
    removed->SetRelativeLocation({4, 0, 0});
    actor->RemoveComponent(removed);
    world.FinalizeWorldGT();
    auto* replacement = actor->AddComponent<SceneComponent>();
    EXPECT_EQ(replacement->GetWorldTransformId().Index, stale.Index);
    EXPECT_NE(replacement->GetWorldTransformId().Generation, stale.Generation);
    const vector<WorldTransformUpdate> invalid{{stable, {{7, 0, 0}, Eigen::Quaternionf::Identity(), Eigen::Vector3f::Ones()}}, {stale, {}}};
    EXPECT_FALSE(world.SetLocalTransforms(invalid));
    EXPECT_FLOAT_EQ(survivor->GetRelativeLocation().x(), 0);
    replacement->SetRelativeLocation({8, 0, 0});
    test::CollectScene(world, render, batch);
    ASSERT_EQ(batch.CreateTransforms.size(), 1u);
    EXPECT_FLOAT_EQ(batch.CreateTransforms[0].Local.Translation[0], 8);
    EXPECT_EQ(batch.RemoveTransforms.size(), 1u);
    world.RequestReconnect();
    test::CollectScene(world, render, batch);
    EXPECT_EQ(survivor->GetWorldTransformId(), stable);
    ASSERT_TRUE(world.SetLocalTransforms({invalid.data(), 1}));
    test::CollectScene(world, render, batch);
    ASSERT_EQ(batch.LocalTransforms.size(), 1u);
    EXPECT_EQ(batch.LocalTransforms[0].Id, survivor->GetSceneTransformId());
    EXPECT_FLOAT_EQ(batch.LocalTransforms[0].Local.Translation[0], 7);
}

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

TEST(SceneUpdates, OrdinaryComponentsKeepTheirLifecycleAndMirrorOnlySceneHierarchy) {
    vector<string> events;
    Application app;
    RenderSystem render{&app, 1};
    test::ScopedWorld world;
    test::ConnectWorld(world, render);
    auto* actor = world.SpawnActor();
    actor->AddComponent<LifecycleComponent>(events);
    auto* scene = actor->AddComponent<SceneComponent>();
    scene->SetRelativeLocation({1, 2, 3});
    world.Tick(0.01f);
    SceneUpdateBatch batch;
    test::CollectScene(world, render, batch);
    EXPECT_TRUE(batch.CreateShapes.empty());
    ASSERT_EQ(batch.CreateTransforms.size(), 1u);
    EXPECT_EQ(batch.CreateTransforms[0].Id, scene->GetSceneTransformId());
    world.DestroyActor(actor);
    test::CollectScene(world, render, batch);
    EXPECT_TRUE(batch.RemoveShapes.empty());
    EXPECT_EQ(batch.RemoveTransforms.size(), 1u);
    EXPECT_EQ(events, (vector<string>{"register", "tick", "unregister"}));
}

class RenderSceneComponent final : public RenderComponent {
public:
    explicit RenderSceneComponent(vector<string>& events) : _events(events) { SetTransformNotificationEnabled(true); }
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
    void CollectRenderUpdates(SceneCapture&, RenderDirtyFlags dirty) override {
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
    EXPECT_TRUE(batch.CreateShapes.empty());
    EXPECT_EQ(batch.CreateTransforms.size(), 1u);
    component->MarkRenderStateDirty();
    actor->RemoveComponent(component);
    test::CollectScene(world, render, batch);
    EXPECT_TRUE(batch.RemoveShapes.empty());
    EXPECT_EQ(batch.RemoveTransforms.size(), 1u);
    EXPECT_EQ(events, (vector<string>{"create", "register", "collect", "destroy", "unregister"}));
}

struct Collection {
    ShapeId Id;
    RenderDirtyFlags Dirty;
    Eigen::Matrix4f WorldMatrix;
};

class MultiEntrySource final : public RenderComponent {
public:
    ShapeId First, Second;
    LightId Light;
    void SetValue(float value) {
        Value = value;
        MarkRenderStateDirty();
    }

protected:
    void CreateRenderState(SceneWriter& writer) override {
        First = writer.CreateShape();
        Second = writer.CreateShape();
        Light = writer.CreateLight();
        MarkRenderStateDirty();
    }
    void DestroyRenderState(SceneWriter& writer) override {
        writer.RemoveShape(First);
        writer.RemoveShape(Second);
        writer.RemoveLight(Light);
    }
    void CollectRenderUpdates(SceneCapture& capture, RenderDirtyFlags) override {
        Eigen::Matrix4f matrix = GetWorldMatrix();
        matrix(0, 3) += Value;
        capture.CaptureShape(First).SetStaticMesh({}, matrix);
        matrix(0, 3) += 100;
        capture.CaptureShape(Second).SetStaticMesh({}, matrix);
        PointLightData light;
        light.Common.Intensity = Value;
        capture.SetLight(Light, light);
    }

private:
    float Value{1};
};

TEST(SceneUpdates, OneSourceCapturesMultipleEntriesAndDirtySourcesSurviveCompaction) {
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    const auto sceneId = test::ConnectWorld(world, renderer);
    auto* actor = world.SpawnActor();
    auto* first = actor->AddComponent<MultiEntrySource>();
    auto* removed = actor->AddComponent<MultiEntrySource>();
    auto* moved = actor->AddComponent<MultiEntrySource>();
    SceneUpdateBatch batch;
    test::CollectScene(world, renderer, batch);
    ASSERT_EQ(batch.MeshStates.size(), 6u);
    first->SetValue(3);
    moved->SetValue(7);
    actor->RemoveComponent(removed);
    test::CollectScene(world, renderer, batch);
    EXPECT_EQ(batch.MeshStates.size(), 4u);
    EXPECT_EQ(batch.RemoveShapes.size(), 2u);
    EXPECT_EQ(batch.Lights.Count(), 2u);
    const auto scene = renderer.GetSceneRT(sceneId);
    EXPECT_FLOAT_EQ(scene->GetStaticMesh(first->First)->LocalToWorld(0, 3), 3);
    EXPECT_FLOAT_EQ(scene->GetStaticMesh(moved->Second)->LocalToWorld(0, 3), 107);
    EXPECT_FLOAT_EQ(scene->GetLight(moved->Light)->Intensity, 7);
}

TEST(SceneUpdates, RepeatedWorldCapturesAndRemovalBeforeSealKeepOnlyFinalValues) {
    Application app;
    RenderSystem renderer{&app, 1};
    test::ScopedWorld world;
    const auto sceneId = test::ConnectWorld(world, renderer);
    auto* actor = world.SpawnActor();
    auto* component = actor->AddComponent<StaticMeshComponent>();
    const auto canceled = component->GetShapeId();
    component->SetRelativeLocation({1, 0, 0});
    world.FinalizeWorldGT();
    world.CollectRenderUpdates();
    component->SetRelativeLocation({2, 0, 0});
    world.FinalizeWorldGT();
    world.CollectRenderUpdates();
    actor->RemoveComponent(component);
    world.FinalizeWorldGT();
    component = actor->AddComponent<StaticMeshComponent>();
    EXPECT_EQ(component->GetShapeId().Index, canceled.Index);
    EXPECT_GT(component->GetShapeId().Generation, canceled.Generation);
    component->SetRelativeLocation({3, 0, 0});
    test::PrepareScene(world, renderer, 0);
    const auto& first = test::SceneBatch(renderer, sceneId, 0);
    ASSERT_EQ(first.CreateShapes.size(), 1u);
    ASSERT_EQ(first.MeshStates.size(), 1u);
    EXPECT_TRUE(first.RemoveShapes.empty());
    EXPECT_TRUE(first.Transforms.empty());
    ASSERT_EQ(first.CreateTransforms.size(), 1u);
    EXPECT_EQ(first.MeshStates[0].Transform, first.CreateTransforms[0].Id);
    EXPECT_FLOAT_EQ(first.CreateTransforms[0].Local.Translation[0], 3);
    test::ConsumeFrame(renderer, 0);
    test::CompleteFrame(renderer, 0);

    component->SetRelativeLocation({4, 0, 0});
    world.FinalizeWorldGT();
    world.CollectRenderUpdates();
    component->SetRelativeLocation({5, 0, 0});
    component->MarkRenderStateDirty();
    world.FinalizeWorldGT();
    world.CollectRenderUpdates();
    component->SetRelativeLocation({6, 0, 0});
    test::PrepareScene(world, renderer, 0);
    const auto& second = test::SceneBatch(renderer, sceneId, 0);
    EXPECT_TRUE(second.CreateShapes.empty());
    EXPECT_TRUE(second.Transforms.empty());
    ASSERT_EQ(second.MeshStates.size(), 1u);
    ASSERT_EQ(second.LocalTransforms.size(), 1u);
    EXPECT_EQ(second.MeshStates[0].Transform, second.LocalTransforms[0].Id);
    EXPECT_FLOAT_EQ(second.LocalTransforms[0].Local.Translation[0], 6);
    test::ConsumeFrame(renderer, 0);
    EXPECT_FLOAT_EQ(renderer.GetSceneRT(sceneId)->GetStaticMesh(component->GetShapeId())->LocalToWorld(0, 3), 6);
    test::CompleteFrame(renderer, 0);
}

TEST(SceneUpdates, IndexedLightRowsSurviveTypeChangesSwapRemovalAndSlotReuse) {
    Application app;
    RenderSystem renderer{&app, 2};
    const auto sceneId = renderer.CreateSceneGT();
    auto writer = renderer.GetSceneWriterGT(sceneId);
    vector<LightId> ids;
    LightSceneData expected;
    for (size_t i = 0; i < 96; ++i) ids.push_back(writer->CreateLight());
    uint32_t random = 193;
    for (uint32_t round = 0; round < 32; ++round) {
        for (uint32_t change = 0; change < 31; ++change) {
            random = random * 1664525u + 1013904223u;
            auto& id = ids[(random >> 8) % ids.size()];
            if ((random & 7) == 0) {
                expected.Remove(id);
                writer->RemoveLight(id);
                id = writer->CreateLight();
            } else {
                LightCommonData common;
                common.Intensity = float(round * 31 + change);
                LightData value;
                switch ((random >> 16) % 4) {
                    case 0: value = DirectionalLightData{common}; break;
                    case 1: value = PointLightData{common, {}}; break;
                    case 2: value = SpotLightData{common, {}}; break;
                    default: value = RectLightData{common}; break;
                }
                expected.Set(id, value);
                writer->SetLight(id, value);
            }
        }
        const auto flight = round % 2;
        renderer.SealFrameGT(flight);
        test::ConsumeFrame(renderer, flight);
        const auto& actual = renderer.GetSceneRT(sceneId)->GetLights();
        EXPECT_EQ(actual.Count(), expected.Count());
        for (const auto id : ids) {
            const auto target = expected.GetLight(id);
            const auto light = actual.GetLight(id);
            ASSERT_EQ(bool(light), bool(target));
            if (light) EXPECT_FLOAT_EQ(light->Intensity, target->Intensity);
            EXPECT_EQ(bool(actual.GetDirectionalLight(id)), bool(expected.GetDirectionalLight(id)));
            EXPECT_EQ(bool(actual.GetPointLight(id)), bool(expected.GetPointLight(id)));
            EXPECT_EQ(bool(actual.GetSpotLight(id)), bool(expected.GetSpotLight(id)));
            EXPECT_EQ(bool(actual.GetRectLight(id)), bool(expected.GetRectLight(id)));
        }
        test::CompleteFrame(renderer, flight);
    }
}

TEST(SceneUpdates, WriterLightSnapshotsSurviveTypeChangesRemovalAndIdentityReuse) {
    Application app;
    RenderSystem renderer{&app, 3};
    const auto sceneId = renderer.CreateSceneGT();
    auto writer = renderer.GetSceneWriterGT(sceneId);
    const auto original = writer->CreateLight();
    DirectionalLightData directional;
    directional.Common.Intensity = 10;
    writer->SetLight(original, directional);
    renderer.SealFrameGT(0);

    PointLightData point;
    point.Common.Intensity = 20;
    writer->SetLight(original, point);
    renderer.SealFrameGT(1);

    writer->RemoveLight(original);
    const auto replacement = writer->CreateLight();
    EXPECT_EQ(replacement.Index, original.Index);
    EXPECT_GT(replacement.Generation, original.Generation);
    SpotLightData spot;
    spot.Common.Intensity = 30;
    writer->SetLight(replacement, spot);
    renderer.SealFrameGT(2);

    const auto& first = test::SceneBatch(renderer, sceneId, 0);
    EXPECT_TRUE(first.LightsChanged);
    ASSERT_EQ(first.Lights.DirectionalLights.Size(), 1u);
    EXPECT_FLOAT_EQ(first.Lights.GetDirectionalLight(original)->Common.Intensity, 10);
    EXPECT_FLOAT_EQ(test::SceneBatch(renderer, sceneId, 1).Lights.GetPointLight(original)->Common.Intensity, 20);
    for (uint32_t flight = 0; flight < 3; ++flight) {
        test::ConsumeFrame(renderer, flight);
        const auto scene = renderer.GetSceneRT(sceneId);
        ASSERT_EQ(scene->GetLights().Count(), 1u);
        const auto id = flight == 2 ? replacement : original;
        EXPECT_FLOAT_EQ(scene->GetLight(id)->Intensity, static_cast<float>((flight + 1) * 10));
        if (flight == 2) EXPECT_FALSE(scene->ContainsLight(original));
        test::CompleteFrame(renderer, flight);
    }

    writer->RemoveLight(replacement);
    renderer.SealFrameGT(0);
    EXPECT_TRUE(test::SceneBatch(renderer, sceneId, 0).LightsChanged);
    EXPECT_TRUE(test::SceneBatch(renderer, sceneId, 0).Lights.Empty());
    test::ConsumeFrame(renderer, 0);
    EXPECT_TRUE(renderer.GetSceneRT(sceneId)->GetLights().Empty());
    test::CompleteFrame(renderer, 0);

    const auto reserved = writer->CreateLight();
    writer->RemoveLight(reserved);
    renderer.SealFrameGT(1);
    EXPECT_TRUE(test::SceneBatch(renderer, sceneId, 1).Empty());
    test::ConsumeFrame(renderer, 1);
    EXPECT_TRUE(renderer.GetSceneRT(sceneId)->GetLights().Empty());
    test::CompleteFrame(renderer, 1);
}

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
    void CollectPrimitiveUpdates(ShapeCapture&, RenderDirtyFlags dirty) override {
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
    void CollectPrimitiveUpdates(ShapeCapture&, RenderDirtyFlags) override {
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
