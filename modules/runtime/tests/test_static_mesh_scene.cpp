#include "scene_test_support.h"
#include <gtest/gtest.h>

#include <limits>
#include <random>
#include <thread>

#include <radray/runtime/application.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>

namespace radray {
namespace {

class CpuMesh final : public StaticMesh {
public:
    using StaticMesh::StaticMesh;
    void OnUnload(AssetManager&) override {}
};

AssetId CpuMeshId(uint32_t value) {
    return AssetId{value, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
}

unique_ptr<CpuMesh> MakeCpuMesh(const Eigen::Vector3f& lower, const Eigen::Vector3f& upper, vector<StaticMeshSection> sections) {
    const array<float, 9> vertices{lower.x(), lower.y(), lower.z(), upper.x(), upper.y(), upper.z(), 0, 0, 0};
    const array<uint32_t, 3> indices{0, 1, 2};
    MeshResource mesh;
    mesh.Bins.emplace_back(std::as_bytes(std::span{vertices}));
    mesh.Bins.emplace_back(std::as_bytes(std::span{indices}));
    MeshPrimitive primitive;
    primitive.VertexCount = 3;
    primitive.VertexBuffers.push_back({"POSITION", 0, 0, VertexDataType::FLOAT, 3, 0, 12});
    primitive.IndexBuffer = {1, 3, 0, 4};
    mesh.Primitives.push_back(std::move(primitive));
    return make_unique<CpuMesh>(std::move(mesh), std::move(sections), lower, upper, GpuMesh{});
}

void ExpectBounds(const StaticMeshSceneView& view, const Eigen::Vector3f& localMin, const Eigen::Vector3f& localMax,
                  const Eigen::Matrix4f& transform) {
    Eigen::Vector3f lower = Eigen::Vector3f::Constant(std::numeric_limits<float>::infinity());
    Eigen::Vector3f upper = -lower;
    for (uint32_t corner = 0; corner < 8; ++corner) {
        Eigen::Vector4f p{(corner & 1) ? localMax.x() : localMin.x(),
                          (corner & 2) ? localMax.y() : localMin.y(),
                          (corner & 4) ? localMax.z() : localMin.z(), 1};
        Eigen::Vector3f transformed = (transform * p).head<3>();
        lower = lower.cwiseMin(transformed);
        upper = upper.cwiseMax(transformed);
    }
    EXPECT_TRUE(view.LocalToWorld.isApprox(transform));
    EXPECT_TRUE(view.WorldBoundsMin.isApprox(lower, 1e-5f));
    EXPECT_TRUE(view.WorldBoundsMax.isApprox(upper, 1e-5f));
}

class StaticMeshScene : public testing::Test {
protected:
    StreamingAssetRef<StaticMesh> Mesh(uint32_t id, Eigen::Vector3f lower = {-1, -2, -3}, Eigen::Vector3f upper = {2, 3, 4},
                                       vector<StaticMeshSection> sections = {{0, 0, 3, 0, 2}}) {
        return Assets.AddReady<StaticMesh>(CpuMeshId(id), MakeCpuMesh(lower, upper, std::move(sections)));
    }

    StaticMeshComponent* Add(StreamingAssetRef<StaticMesh> mesh) {
        auto* component = Owner->AddComponent<StaticMeshComponent>();
        component->SetStaticMesh(std::move(mesh));
        return component;
    }

    void Flush() {
        test::CollectScene(GameWorld, Render, Batch);
        Data.Apply(Batch);
        Batch.Clear();
    }

    Application App;
    AssetManager Assets;
    RenderSystem Render{&App, 3};
    test::ScopedWorld GameWorld;
    SceneId RenderId{test::ConnectWorld(GameWorld, Render)};
    RenderScene Data;
    SceneUpdateBatch Batch;
    Actor* Owner{GameWorld.SpawnActor()};
};

TEST_F(StaticMeshScene, CreateCombinesStateAndFinalTransform) {
    auto* component = Add(Mesh(1));
    for (int i = 0; i < 100; ++i) {
        component->SetRelativeLocation({static_cast<float>(i), 10, 20});
        component->MarkRenderStateDirty();
    }
    const ShapeId id = component->GetShapeId();
    test::CollectScene(GameWorld, Render, Batch);
    ASSERT_EQ(Batch.CreateShapes.size(), 1u);
    ASSERT_EQ(Batch.MeshStates.size(), 1u);
    EXPECT_TRUE(Batch.Transforms.empty());
    EXPECT_EQ(Batch.MeshStates[0].Id, id);
    EXPECT_EQ(Batch.MeshStates[0].Transform, component->GetSceneTransformId());
    ASSERT_EQ(Batch.CreateTransforms.size(), 1u);
    EXPECT_FLOAT_EQ(Batch.CreateTransforms[0].Local.Translation[0], 99);
    Data.Apply(Batch);
    Batch.Clear();
    auto view = Data.GetStaticMesh(id);
    ASSERT_TRUE(view);
    EXPECT_EQ(view->Mesh.MeshAssetId, CpuMeshId(1));
    ASSERT_EQ(view->Mesh.GetSections().size(), 1u);
    EXPECT_EQ(view->Mesh.GetSections()[0].IndexCount, 3u);
    ExpectBounds(*view, {-1, -2, -3}, {2, 3, 4}, component->GetWorldMatrix());
    EXPECT_FALSE(view->ReverseCulling);
    ASSERT_EQ(Data.GetStaticMeshes().size(), 1u);
    EXPECT_EQ(Data.GetStaticMeshes()[0], id);
}

TEST_F(StaticMeshScene, WriterCoalescesInterleavedUpdatesAcrossFlightsAndSlotReuse) {
    const auto sceneId = Render.CreateSceneGT();
    auto* writer = Render.GetSceneWriterGT(sceneId).Get();
    const auto original = writer->CreateShape();
    const auto survivor = writer->CreateShape();
    auto mesh = Mesh(1);
    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    writer->SetStaticMesh(original, mesh, transform);
    writer->SetStaticMesh(survivor, mesh, transform);
    Render.SealFrameGT(0);

    transform(0, 3) = 10;
    writer->SetTransform(original, transform);
    writer->SetStaticMesh(original, {}, Eigen::Matrix4f::Identity());
    transform(0, 3) = 20;
    writer->SetTransform(original, transform);
    Render.SealFrameGT(1);
    const auto& replaced = test::SceneBatch(Render, sceneId, 1);
    ASSERT_EQ(replaced.MeshStates.size(), 1u);
    EXPECT_TRUE(replaced.Transforms.empty());
    EXPECT_TRUE(replaced.MeshStates[0].Mesh.MeshAssetId.IsEmpty());
    EXPECT_FLOAT_EQ(replaced.MeshStates[0].LocalToWorld(0, 3), 20);

    writer->SetTransform(original, transform);
    transform(0, 3) = 30;
    writer->SetTransform(survivor, transform);
    writer->RemoveShape(original);
    const auto replacement = writer->CreateShape();
    EXPECT_EQ(replacement.Index, original.Index);
    EXPECT_GT(replacement.Generation, original.Generation);
    transform(0, 3) = 40;
    writer->SetStaticMesh(replacement, mesh, transform);
    transform(0, 3) = 50;
    writer->SetTransform(replacement, transform);
    Render.SealFrameGT(2);
    const auto& reused = test::SceneBatch(Render, sceneId, 2);
    EXPECT_EQ(reused.RemoveShapes, vector<ShapeId>{original});
    EXPECT_EQ(reused.CreateShapes, vector<ShapeId>{replacement});
    ASSERT_EQ(reused.MeshStates.size(), 1u);
    ASSERT_EQ(reused.Transforms.size(), 1u);

    test::ConsumeFrame(Render, 0);
    EXPECT_EQ(Render.GetSceneRT(sceneId)->GetStaticMesh(original)->Mesh.MeshAssetId, mesh.GetAssetId());
    EXPECT_FLOAT_EQ(Render.GetSceneRT(sceneId)->GetStaticMesh(original)->LocalToWorld(0, 3), 0);
    test::ConsumeFrame(Render, 1);
    EXPECT_FALSE(Render.GetSceneRT(sceneId)->GetStaticMesh(original)->Mesh.GetRenderMesh());
    EXPECT_FLOAT_EQ(Render.GetSceneRT(sceneId)->GetStaticMesh(original)->LocalToWorld(0, 3), 20);
    test::ConsumeFrame(Render, 2);
    EXPECT_FALSE(Render.GetSceneRT(sceneId)->ContainsShape(original));
    EXPECT_EQ(Render.GetSceneRT(sceneId)->GetStaticMesh(replacement)->Mesh.MeshAssetId, mesh.GetAssetId());
    EXPECT_FLOAT_EQ(Render.GetSceneRT(sceneId)->GetStaticMesh(replacement)->LocalToWorld(0, 3), 50);
    EXPECT_FLOAT_EQ(Render.GetSceneRT(sceneId)->GetStaticMesh(survivor)->LocalToWorld(0, 3), 30);
    for (uint32_t flight = 0; flight < 3; ++flight) test::CompleteFrame(Render, flight);
}

TEST_F(StaticMeshScene, WriterCancelsPendingCreatesAndMeshStatesWithoutLosingSurvivors) {
    auto firstMesh = Mesh(1);
    auto finalMesh = Mesh(2);
    for (size_t removed = 0; removed < 3; ++removed) {
        SCOPED_TRACE(removed);
        const auto sceneId = Render.CreateSceneGT();
        auto* writer = Render.GetSceneWriterGT(sceneId).Get();
        array<ShapeId, 3> ids;
        Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
        for (auto& id : ids) {
            id = writer->CreateShape();
            writer->SetStaticMesh(id, firstMesh, transform);
        }
        const auto canceled = ids[removed];
        writer->RemoveShape(canceled);
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i == removed) continue;
            writer->SetStaticMesh(ids[i], finalMesh, transform);
            transform(0, 3) = static_cast<float>(i + 10);
            writer->SetTransform(ids[i], transform);
        }
        ids[removed] = writer->CreateShape();
        EXPECT_EQ(ids[removed].Index, canceled.Index);
        EXPECT_GT(ids[removed].Generation, canceled.Generation);
        writer->SetStaticMesh(ids[removed], {}, Eigen::Matrix4f::Identity());
        Render.SealFrameGT(0);
        const auto& batch = test::SceneBatch(Render, sceneId, 0);
        EXPECT_EQ(batch.CreateShapes.size(), 3u);
        EXPECT_EQ(batch.MeshStates.size(), 3u);
        EXPECT_TRUE(batch.Transforms.empty());
        EXPECT_TRUE(batch.RemoveShapes.empty());
        test::ConsumeFrame(Render, 0);
        const auto scene = Render.GetSceneRT(sceneId);
        EXPECT_FALSE(scene->ContainsShape(canceled));
        EXPECT_EQ(scene->GetStaticMeshes().size(), 3u);
        for (size_t i = 0; i < ids.size(); ++i) {
            const auto view = scene->GetStaticMesh(ids[i]);
            ASSERT_TRUE(view);
            EXPECT_EQ(view->Mesh.RenderData.Get(), i == removed ? nullptr : &finalMesh->GetRenderData());
            EXPECT_FLOAT_EQ(view->LocalToWorld(0, 3), i == removed ? 0 : static_cast<float>(i + 10));
        }
        test::CompleteFrame(Render, 0);
    }
}

TEST_F(StaticMeshScene, WriterMergesMeshChangesIntoPendingTransformsAndCancelsRemoval) {
    auto mesh = Mesh(1);
    for (size_t removed = 0; removed < 3; ++removed) {
        SCOPED_TRACE(removed);
        const auto sceneId = Render.CreateSceneGT();
        auto* writer = Render.GetSceneWriterGT(sceneId).Get();
        array<ShapeId, 3> ids;
        Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
        for (auto& id : ids) {
            id = writer->CreateShape();
            writer->SetStaticMesh(id, mesh, transform);
        }
        Render.SealFrameGT(0);
        for (const auto id : ids) writer->SetTransform(id, transform);
        writer->RemoveShape(ids[removed]);
        const size_t rebound = (removed + 1) % ids.size();
        writer->SetStaticMesh(ids[rebound], {}, transform);
        writer->SetStaticMesh(ids[rebound], mesh, transform);
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i == removed) continue;
            transform(0, 3) = static_cast<float>(i + 20);
            writer->SetTransform(ids[i], transform);
        }
        Render.SealFrameGT(1);
        const auto& batch = test::SceneBatch(Render, sceneId, 1);
        EXPECT_EQ(batch.RemoveShapes, vector<ShapeId>{ids[removed]});
        EXPECT_TRUE(batch.CreateShapes.empty());
        ASSERT_EQ(batch.MeshStates.size(), 1u);
        EXPECT_EQ(batch.MeshStates[0].Id, ids[rebound]);
        EXPECT_EQ(batch.Transforms.size(), 1u);
        test::ConsumeFrame(Render, 0);
        for (const auto id : ids) {
            EXPECT_FLOAT_EQ(Render.GetSceneRT(sceneId)->GetStaticMesh(id)->LocalToWorld(0, 3), 0);
        }
        test::ConsumeFrame(Render, 1);
        EXPECT_FALSE(Render.GetSceneRT(sceneId)->ContainsShape(ids[removed]));
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i == removed) continue;
            EXPECT_FLOAT_EQ(Render.GetSceneRT(sceneId)->GetStaticMesh(ids[i])->LocalToWorld(0, 3), static_cast<float>(i + 20));
        }
        test::CompleteFrame(Render, 0);
        test::CompleteFrame(Render, 1);
    }
}

TEST_F(StaticMeshScene, InstancesAcrossScenesShareAssetRenderDataThroughRebindAndRetirement) {
    auto mesh = Mesh(1);
    auto replacementMesh = Mesh(2, {-10, -20, -30}, {30, 40, 50});
    const auto* sharedData = &mesh->GetRenderData();
    const auto firstScene = Render.CreateSceneGT();
    const auto secondScene = Render.CreateSceneGT();
    auto* first = Render.GetSceneWriterGT(firstScene).Get();
    auto* second = Render.GetSceneWriterGT(secondScene).Get();
    const auto moving = first->CreateShape();
    const auto stationary = first->CreateShape();
    const auto other = second->CreateShape();
    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    first->SetStaticMesh(moving, mesh, transform);
    first->SetStaticMesh(stationary, mesh, transform);
    second->SetStaticMesh(other, mesh, transform);
    Render.SealFrameGT(0);
    test::ConsumeFrame(Render, 0);
    EXPECT_EQ(Render.GetSceneRT(firstScene)->GetStaticMesh(moving)->Mesh.RenderData.Get(), sharedData);
    EXPECT_EQ(Render.GetSceneRT(firstScene)->GetStaticMesh(stationary)->Mesh.RenderData.Get(), sharedData);
    EXPECT_EQ(Render.GetSceneRT(secondScene)->GetStaticMesh(other)->Mesh.RenderData.Get(), sharedData);

    transform(0, 3) = 15;
    first->SetTransform(stationary, transform);
    first->SetStaticMesh(moving, replacementMesh, transform);
    second->RemoveShape(other);
    mesh = {};
    Render.SealFrameGT(1);
    Assets.Pump();
    EXPECT_EQ(Render.GetSceneRT(secondScene)->GetStaticMesh(other)->Mesh.RenderData.Get(), sharedData);
    test::ConsumeFrame(Render, 1);
    EXPECT_EQ(Render.GetSceneRT(firstScene)->GetStaticMesh(moving)->Mesh.RenderData.Get(), &replacementMesh->GetRenderData());
    EXPECT_EQ(Render.GetSceneRT(firstScene)->GetStaticMesh(stationary)->Mesh.RenderData.Get(), sharedData);
    ExpectBounds(*Render.GetSceneRT(firstScene)->GetStaticMesh(stationary), {-1, -2, -3}, {2, 3, 4}, transform);
    for (uint32_t flight = 0; flight < 2; ++flight) test::CompleteFrame(Render, flight);
    Assets.Pump();
    EXPECT_EQ(Render.GetSceneRT(firstScene)->GetStaticMesh(stationary)->Mesh.GetSections().size(), 1u);
}

TEST_F(StaticMeshScene, MovingOneObjectPreservesMeshDescriptionAndOtherObjects) {
    auto mesh = Mesh(1);
    auto* moving = Add(mesh);
    auto* stationary = Add(mesh);
    stationary->SetRelativeLocation({500, 0, 0});
    Flush();
    const auto movingId = moving->GetShapeId();
    const auto stationaryId = stationary->GetShapeId();
    const auto* sections = Data.GetStaticMesh(movingId)->Mesh.GetSections().data();
    const auto* otherSections = Data.GetStaticMesh(stationaryId)->Mesh.GetSections().data();
    const Eigen::Matrix4f otherTransform = Data.GetStaticMesh(stationaryId)->LocalToWorld;
    const Eigen::Vector3f otherBounds = Data.GetStaticMesh(stationaryId)->WorldBoundsMin;
    for (int i = 0; i < 100; ++i) moving->SetRelativeLocation({static_cast<float>(i), 0, 0});
    test::CollectScene(GameWorld, Render, Batch);
    EXPECT_TRUE(Batch.MeshStates.empty());
    ASSERT_EQ(Batch.LocalTransforms.size(), 1u);
    EXPECT_EQ(Batch.LocalTransforms[0].Id, moving->GetSceneTransformId());
    Data.Apply(Batch);
    Batch.Clear();
    EXPECT_EQ(Data.GetStaticMesh(movingId)->Mesh.GetSections().data(), sections);
    EXPECT_EQ(Data.GetStaticMesh(stationaryId)->Mesh.GetSections().data(), otherSections);
    EXPECT_TRUE(Data.GetStaticMesh(stationaryId)->LocalToWorld.isApprox(otherTransform));
    EXPECT_TRUE(Data.GetStaticMesh(stationaryId)->WorldBoundsMin.isApprox(otherBounds));
    ExpectBounds(*Data.GetStaticMesh(movingId), {-1, -2, -3}, {2, 3, 4}, moving->GetWorldMatrix());
    test::CollectScene(GameWorld, Render, Batch);
    EXPECT_TRUE(Batch.Empty());
}

TEST_F(StaticMeshScene, ReplacementUsesNewBoundsAndFinalTransformWithoutChangingId) {
    auto* component = Add(Mesh(1));
    Flush();
    const auto id = component->GetShapeId();
    component->SetStaticMesh(Mesh(2, {-10, -20, -30}, {30, 40, 50}, {{0, 0, 1, 0, 0}, {0, 1, 2, 1, 2}}));
    component->SetRelativeScale({-2, 3, 4});
    component->SetRelativeLocation({30, 50, 70});
    test::CollectScene(GameWorld, Render, Batch);
    EXPECT_TRUE(Batch.CreateShapes.empty());
    EXPECT_TRUE(Batch.Transforms.empty());
    ASSERT_EQ(Batch.MeshStates.size(), 1u);
    EXPECT_EQ(Batch.MeshStates[0].Id, id);
    Data.Apply(Batch);
    Batch.Clear();
    auto view = Data.GetStaticMesh(id);
    ASSERT_TRUE(view);
    EXPECT_EQ(view->Mesh.MeshAssetId, CpuMeshId(2));
    ASSERT_EQ(view->Mesh.GetSections().size(), 2u);
    EXPECT_EQ(view->Mesh.GetSections()[1].FirstIndex, 1u);
    ExpectBounds(*view, {-10, -20, -30}, {30, 40, 50}, component->GetWorldMatrix());
    EXPECT_TRUE(view->ReverseCulling);
    EXPECT_EQ(component->GetShapeId(), id);
}

TEST_F(StaticMeshScene, ParentRotationNonuniformScaleAndReflectionsUpdateBoundsAndWinding) {
    auto* parent = Owner->AddComponent<SceneComponent>();
    parent->SetRelativeScale({2, 3, 4});
    parent->SetRelativeRotation(Eigen::Quaternionf{Eigen::AngleAxisf{0.7f, Eigen::Vector3f::UnitY()}});
    auto* component = Add(Mesh(1));
    component->RequestReparent(parent);
    component->SetRelativeRotation(Eigen::Quaternionf{Eigen::AngleAxisf{0.3f, Eigen::Vector3f::UnitZ()}});
    component->SetRelativeLocation({3, 4, 5});
    Flush();
    const auto id = component->GetShapeId();
    for (const Eigen::Vector3f scale : {Eigen::Vector3f{-1, 2, 3}, Eigen::Vector3f{-1, -2, 3}, Eigen::Vector3f{0, 2, 3}, Eigen::Vector3f{-1e-20f, 1e-20f, 1e-20f}}) {
        component->SetRelativeScale(scale);
        test::CollectScene(GameWorld, Render, Batch);
        EXPECT_TRUE(Batch.MeshStates.empty());
        ASSERT_EQ(Batch.LocalTransforms.size(), 1u);
        Data.Apply(Batch);
        Batch.Clear();
        auto view = Data.GetStaticMesh(id);
        ASSERT_TRUE(view);
        const auto matrix = component->GetWorldMatrix();
        ExpectBounds(*view, {-1, -2, -3}, {2, 3, 4}, matrix);
        EXPECT_EQ(view->ReverseCulling, static_cast<double>(scale.x()) * scale.y() * scale.z() < 0);
    }
}

TEST_F(StaticMeshScene, EmptyBindingAndInvalidCpuMeshClearPreviousGeometry) {
    auto* component = Add(Mesh(1));
    Flush();
    const auto id = component->GetShapeId();
    component->SetStaticMesh({});
    component->SetRelativeLocation({5, 6, 7});
    Flush();
    auto view = Data.GetStaticMesh(id);
    ASSERT_TRUE(view);
    EXPECT_TRUE(view->Mesh.MeshAssetId.IsEmpty());
    EXPECT_TRUE(view->Mesh.GetSections().empty());
    EXPECT_FLOAT_EQ(view->LocalToWorld(0, 3), 5);
    auto invalid = Assets.AddReady<StaticMesh>(CpuMeshId(2), make_unique<CpuMesh>(MeshResource{}, vector<StaticMeshSection>{},
                                                                               Eigen::Vector3f::Zero(), Eigen::Vector3f::Zero(), GpuMesh{}));
    component->SetStaticMesh(invalid);
    Flush();
    EXPECT_TRUE(Data.GetStaticMesh(id)->Mesh.GetSections().empty());
    EXPECT_EQ(Data.GetStaticMesh(id)->Mesh.MeshAssetId, CpuMeshId(2));
    component->SetStaticMesh(Mesh(3));
    Flush();
    EXPECT_EQ(Data.GetStaticMesh(id)->Mesh.GetSections().size(), 1u);
    EXPECT_EQ(component->GetShapeId(), id);
}

TEST_F(StaticMeshScene, MeshWithoutSectionsProducesFullPrimitiveDescription) {
    auto* component = Add(Mesh(1, {-1, -1, -1}, {1, 1, 1}, {}));
    Flush();
    const auto& sections = Data.GetStaticMesh(component->GetShapeId())->Mesh.GetSections();
    ASSERT_EQ(sections.size(), 1u);
    EXPECT_EQ(sections[0].PrimitiveIndex, 0u);
    EXPECT_EQ(sections[0].FirstIndex, 0u);
    EXPECT_EQ(sections[0].IndexCount, 3u);
    EXPECT_EQ(sections[0].MaxVertexIndex, 2u);
}

TEST_F(StaticMeshScene, ReadyAutomaticallyPublishesTheLatestTransform) {
    auto* component = Add(Mesh(1));
    Flush();
    auto loading = Assets.Load({.Id = CpuMeshId(2), .Task = []() -> task<AssetLoadResult> {
                                    co_return AssetLoadResult::Success(MakeCpuMesh({-2, -3, -4}, {3, 4, 5}, {{0, 0, 3, 0, 2}}));
                                }()})
                       .CastTo<StaticMesh>();
    ASSERT_FALSE(loading.IsReady());
    const auto id = component->GetShapeId();
    component->SetStaticMesh(loading);
    component->SetRelativeLocation({20, 0, 0});
    Flush();
    EXPECT_TRUE(Data.GetStaticMesh(id)->Mesh.GetSections().empty());
    EXPECT_EQ(Data.GetStaticMesh(id)->Mesh.MeshAssetId, CpuMeshId(2));
    Assets.Pump();
    ASSERT_TRUE(loading.IsReady());
    component->SetRelativeLocation({50, 0, 0});
    Flush();
    ASSERT_EQ(Data.GetStaticMesh(id)->Mesh.GetSections().size(), 1u);
    ExpectBounds(*Data.GetStaticMesh(id), {-2, -3, -4}, {3, 4, 5}, component->GetWorldMatrix());
}

TEST_F(StaticMeshScene, DestroyingUnsentMeshLeavesNoTypedPayload) {
    auto* component = Add(Mesh(1));
    component->SetRelativeLocation({100, 0, 0});
    Owner->RemoveComponent(component);
    test::CollectScene(GameWorld, Render, Batch);
    EXPECT_TRUE(Batch.Empty());
    Data.Apply(Batch);
    EXPECT_TRUE(Data.GetStaticMeshes().empty());
}

TEST_F(StaticMeshScene, FlightRetainsAssetAfterSourceDiesBeforeApplyOnAnotherThread) {
    auto* component = Add(Mesh(1));
    const auto id = component->GetShapeId();
    SceneUpdateBatch create;
    test::PrepareScene(GameWorld, Render, 0);
    create = test::SceneBatch(Render, RenderId, 0);
    Owner->RemoveComponent(component);
    Assets.Pump();
    EXPECT_EQ(Assets.GetAssetCount(), 1u);
    SceneUpdateBatch remove;
    test::PrepareScene(GameWorld, Render, 1);
    remove = test::SceneBatch(Render, RenderId, 1);
    EXPECT_TRUE(remove.MeshStates.empty());
    EXPECT_TRUE(remove.Transforms.empty());
    std::thread render([&]() {
        Data.Apply(create);
        auto view = Data.GetStaticMesh(id);
        ASSERT_TRUE(view);
        EXPECT_EQ(view->Mesh.MeshAssetId, CpuMeshId(1));
        ASSERT_EQ(view->Mesh.GetSections().size(), 1u);
        EXPECT_EQ(view->Mesh.GetSections()[0].IndexCount, 3u);
        ASSERT_TRUE(view->Mesh.GetRenderMesh());
        EXPECT_TRUE(view->Mesh.GetRenderMesh()->Draws.empty());
        Data.Apply(remove);
        EXPECT_FALSE(Data.GetStaticMesh(id));
        EXPECT_TRUE(Data.GetStaticMeshes().empty());
    });
    render.join();
    test::ConsumeFrame(Render, 0);
    test::ConsumeFrame(Render, 1);
    test::CompleteFrame(Render, 0);
    test::CompleteFrame(Render, 1);
    Assets.Pump();
    EXPECT_EQ(Assets.GetAssetCount(), 0u);
}

TEST_F(StaticMeshScene, RemovalRepairsDenseListAndReuseCannotExposeOldMesh) {
    vector<StaticMeshComponent*> components;
    for (int i = 0; i < 3; ++i) components.push_back(Add(Mesh(1)));
    Flush();
    const auto oldId = components[1]->GetShapeId();
    Owner->RemoveComponent(components[1]);
    GameWorld.FinalizeWorldGT();
    auto* replacement = Add(Mesh(2));
    const auto newId = replacement->GetShapeId();
    EXPECT_EQ(oldId.Index, newId.Index);
    Flush();
    EXPECT_FALSE(Data.GetStaticMesh(oldId));
    ASSERT_TRUE(Data.GetStaticMesh(newId));
    EXPECT_EQ(Data.GetStaticMesh(newId)->Mesh.MeshAssetId, CpuMeshId(2));
    EXPECT_EQ(Data.GetStaticMeshes().size(), 3u);
    Owner->RemoveComponent(components[2]);
    Owner->RemoveComponent(components[0]);
    Flush();
    ASSERT_EQ(Data.GetStaticMeshes().size(), 1u);
    EXPECT_EQ(Data.GetStaticMeshes()[0], newId);
    Owner->RemoveComponent(replacement);
    Flush();
    EXPECT_TRUE(Data.GetStaticMeshes().empty());
}

TEST_F(StaticMeshScene, ReadOnlyViewsAndEmptyFramesPreservePersistentDescription) {
    auto* component = Add(Mesh(1));
    Flush();
    const auto id = component->GetShapeId();
    const auto* description = &Data.GetStaticMesh(id)->Mesh;
    const auto* sections = description->GetSections().data();
    for (int frame = 0; frame < 8; ++frame) {
        test::CollectScene(GameWorld, Render, Batch);
        EXPECT_TRUE(Batch.Empty());
        Data.Apply(Batch);
        for (int viewIndex = 0; viewIndex < 3; ++viewIndex) {
            const auto view = Data.GetStaticMesh(id);
            ASSERT_TRUE(view);
            EXPECT_EQ(&view->Mesh, description);
            EXPECT_EQ(view->Mesh.GetSections().data(), sections);
        }
    }
}

TEST_F(StaticMeshScene, ReusedFlightsNeverRestoreAnOlderTransform) {
    Application app;
    for (uint32_t count : {1u, 2u, 3u}) {
        RenderSystem render{&app, count};
        test::ScopedWorld world;
        const auto sceneId = test::ConnectWorld(world, render);
        auto* component = world.SpawnActor()->AddComponent<StaticMeshComponent>();
        component->SetStaticMesh(Mesh(1));
        const auto id = component->GetShapeId();
        for (uint32_t frame = 0; frame < count * 4; ++frame) {
            if (frame == 1) component->SetRelativeLocation({15, 0, 0});
            const uint32_t flight = frame % count;
            test::PrepareScene(world, render, flight);
            test::ConsumeFrame(render, flight);
            const auto view = render.GetSceneRT(sceneId)->GetStaticMesh(id);
            ASSERT_TRUE(view);
            EXPECT_FLOAT_EQ(view->LocalToWorld(0, 3), frame == 0 ? 0.0f : 15.0f);
            test::CompleteFrame(render, flight, false);
        }
    }
}

TEST_F(StaticMeshScene, TransformBatchReusesStorageAfterWarmup) {
    auto* component = Add(Mesh(1));
    Flush();
    component->SetRelativeLocation({1, 0, 0});
    test::CollectScene(GameWorld, Render, Batch);
    const auto* storage = Batch.LocalTransforms.data();
    const auto capacity = Batch.LocalTransforms.capacity();
    Data.Apply(Batch);
    Batch.Clear();
    for (int frame = 0; frame < 8; ++frame) {
        component->SetRelativeLocation({static_cast<float>(frame), 0, 0});
        test::CollectScene(GameWorld, Render, Batch);
        EXPECT_TRUE(Batch.MeshStates.empty());
        EXPECT_EQ(Batch.LocalTransforms.data(), storage);
        EXPECT_EQ(Batch.LocalTransforms.capacity(), capacity);
        Data.Apply(Batch);
        Batch.Clear();
    }
}

TEST_F(StaticMeshScene, TransformLocatorsSurviveBatchReorderingRepeatedCollectAndRemoval) {
    auto* parent = Owner->AddComponent<SceneComponent>();
    parent->SetRelativeLocation({100, 0, 0});
    auto* first = Add(Mesh(1));
    auto* second = Add(Mesh(1));
    auto* third = Add(Mesh(1));
    const auto removedShape = first->GetShapeId();
    const auto removedTransform = first->GetSceneTransformId();
    Flush();

    for (auto* component : {first, second, third}) {
        component->SetRelativeLocation({1, 0, 0});
        component->RequestReparent(parent, AttachmentRule::KeepLocal);
    }
    Flush();

    // The previous batch's first/second locators now point at different identities.
    for (auto* component : {second, third}) {
        component->SetRelativeLocation({2, 0, 0});
        component->RequestReparent(nullptr, AttachmentRule::KeepLocal);
    }
    GameWorld.FinalizeWorldGT();
    GameWorld.CollectRenderUpdates();
    Owner->RemoveComponent(first);
    GameWorld.FinalizeWorldGT();
    auto* replacement = Add(Mesh(1));
    EXPECT_EQ(replacement->GetSceneTransformId().Index, removedTransform.Index);
    EXPECT_NE(replacement->GetSceneTransformId().Generation, removedTransform.Generation);
    replacement->SetRelativeLocation({7, 0, 0});
    second->SetRelativeLocation({3, 0, 0});
    third->RequestReparent(parent, AttachmentRule::KeepLocal);
    test::CollectScene(GameWorld, Render, Batch);
    ASSERT_EQ(Batch.LocalTransforms.size(), 2u);
    ASSERT_EQ(Batch.TransformParents.size(), 2u);
    ASSERT_EQ(Batch.CreateTransforms.size(), 1u);
    ASSERT_EQ(Batch.RemoveTransforms.size(), 1u);
    Data.Apply(Batch);
    Batch.Clear();

    EXPECT_FALSE(Data.ContainsShape(removedShape));
    for (auto* component : {second, third, replacement}) {
        auto view = Data.GetStaticMesh(component->GetShapeId());
        ASSERT_TRUE(view);
        ExpectBounds(*view, {-1, -2, -3}, {2, 3, 4}, component->GetWorldMatrix());
    }

    // Cancelling a current record swaps the last record; repeated capture must find it.
    second->SetRelativeLocation({4, 0, 0});
    third->SetRelativeLocation({5, 0, 0});
    second->RequestReparent(parent, AttachmentRule::KeepLocal);
    third->RequestReparent(nullptr, AttachmentRule::KeepLocal);
    GameWorld.FinalizeWorldGT();
    GameWorld.CollectRenderUpdates();
    Owner->RemoveComponent(second);
    GameWorld.FinalizeWorldGT();
    third->SetRelativeLocation({6, 0, 0});
    third->RequestReparent(parent, AttachmentRule::KeepLocal);
    test::CollectScene(GameWorld, Render, Batch);
    ASSERT_EQ(Batch.LocalTransforms.size(), 1u);
    ASSERT_EQ(Batch.TransformParents.size(), 1u);
    EXPECT_EQ(Batch.LocalTransforms[0].Id, third->GetSceneTransformId());
    Data.Apply(Batch);
    Batch.Clear();
    auto view = Data.GetStaticMesh(third->GetShapeId());
    ASSERT_TRUE(view);
    ExpectBounds(*view, {-1, -2, -3}, {2, 3, 4}, third->GetWorldMatrix());
}

TEST(SceneTransform, RandomizedEditsReparentingAndSlotReuseMatchReference) {
    constexpr uint32_t count = 512;
    SceneTransform scene;
    vector<TransformId> ids;
    vector<uint32_t> parents(count, SceneTransform::kNoRow);
    vector<LocalTransform> locals(count);
    vector<Eigen::Matrix4f> expected(count);
    std::mt19937 random{0x51CEu};
    scene.BeginApply();
    for (uint32_t i = 0; i < count; ++i) {
        ids.push_back({i, 1});
        locals[i] = {{float(i), 1, -2}, Eigen::Quaternionf{Eigen::AngleAxisf{float(i) * .03f, Eigen::Vector3f::UnitY()}}, {1, 2, -1}};
        scene.Create(ids[i], locals[i]);
        if (i) {
            parents[i] = (i - 1) / 3;
            scene.SetParent(ids[i], ids[parents[i]]);
        }
    }
    for (uint32_t frame = 0; frame < 400; ++frame) {
        SCOPED_TRACE(frame);
        if (frame) {
            scene.BeginApply();
            for (uint32_t change = 0; change < (frame % 23 == 0 ? count : 20u); ++change) {
                const uint32_t node = random() % count;
                locals[node].Translation[0] = float(random() % 100) * .01f;
                scene.SetLocal(ids[node], locals[node]);
            }
            for (uint32_t change = 0; change < 4; ++change) {
                const uint32_t node = 1 + random() % (count - 1);
                parents[node] = (random() % 3) ? random() % node : SceneTransform::kNoRow;
                scene.SetParent(ids[node], parents[node] == SceneTransform::kNoRow ? TransformId{} : ids[parents[node]]);
            }
            const uint32_t recycled = random() % count;
            scene.Remove(ids[recycled]);
            for (auto& parent : parents) {
                if (parent == recycled) parent = SceneTransform::kNoRow;
            }
            parents[recycled] = SceneTransform::kNoRow;
            ++ids[recycled].Generation;
            scene.Create(ids[recycled], locals[recycled]);
        }
        const auto changed = scene.Evaluate();
        vector<uint32_t> unique(changed.begin(), changed.end());
        std::sort(unique.begin(), unique.end());
        EXPECT_EQ(std::unique(unique.begin(), unique.end()), unique.end());
        for (uint32_t i = 0; i < count; ++i) {
            expected[i] = locals[i].ToMatrix();
            if (parents[i] != SceneTransform::kNoRow) expected[i] = (expected[parents[i]] * expected[i]).eval();
            EXPECT_TRUE(scene.GetWorld(scene.GetRow(ids[i])).isApprox(expected[i], 1e-4f)) << i;
        }
    }
    scene.BeginApply();
    EXPECT_TRUE(scene.Evaluate().empty());
}

TEST(StaticMeshSceneDeathTest, RejectsStaleStateTransformAndTransformWithoutMesh) {
    RenderScene scene;
    SceneUpdateBatch batch;
    batch.CreateShapes.push_back({0, 1});
    batch.MeshStates.push_back({.Id = {0, 1}});
    scene.Apply(batch);
    batch.Clear();
    batch.RemoveShapes.push_back({0, 1});
    batch.CreateShapes.push_back({0, 2});
    batch.MeshStates.push_back({.Id = {0, 2}});
    scene.Apply(batch);
    batch.Clear();
    batch.MeshStates.push_back({.Id = {0, 1}});
    EXPECT_DEATH(scene.Apply(batch), "");
    batch.Clear();
    batch.Transforms.push_back({.Id = {0, 1}});
    EXPECT_DEATH(scene.Apply(batch), "");
    batch.Clear();
    batch.CreateShapes.push_back({1, 1});
    scene.Apply(batch);
    batch.Clear();
    batch.Transforms.push_back({.Id = {1, 1}});
    EXPECT_DEATH(scene.Apply(batch), "");
}

TEST(StaticMeshColumns, DenseColumnsPreserveAffineDataAcrossRemovalAndRebinding) {
    RenderScene scene;
    SceneUpdateBatch batch;
    StaticMeshRenderData geometry;
    geometry.LocalBoundsMin = {-1, -2, -3};
    geometry.LocalBoundsMax = {1, 2, 3};
    Eigen::Matrix4f affine = Eigen::Matrix4f::Identity();
    affine(0, 0) = -2;
    affine(0, 1) = 0.75f;
    affine(2, 3) = 11;
    const AffineTransform packed{affine};
    EXPECT_EQ(sizeof(ShapeTransformUpdate), 56u);
    EXPECT_TRUE(packed.ToMatrix().isApprox(affine));
    for (uint32_t i = 0; i < 3; ++i) {
        const ShapeId id{i, 1};
        batch.CreateShapes.push_back(id);
        auto matrix = affine;
        matrix(0, 3) = float(i * 10);
        batch.MeshStates.push_back({id, {{}, &geometry}, matrix});
    }
    scene.Apply(batch);
    batch.Clear();
    batch.RemoveShapes.push_back({0, 1});
    affine(1, 3) = 17;
    batch.Transforms.push_back({{2, 1}, affine});
    scene.Apply(batch);
    const auto columns = scene.GetStaticMeshColumns();
    ASSERT_EQ(columns.Size(), 2u);
    EXPECT_EQ(columns.Bindings.size(), columns.Size());
    EXPECT_EQ(columns.TransformRows.size(), columns.Size());
    EXPECT_EQ(columns.Bounds.size(), columns.Size());
    EXPECT_EQ(columns.Ids[0], (ShapeId{2, 1}));
    EXPECT_TRUE(columns.Get(0).LocalToWorld.isApprox(affine));
    EXPECT_EQ(columns.Bindings[0].RenderData.Get(), &geometry);
    EXPECT_TRUE(columns.Bounds[0].ReverseCulling);
    EXPECT_TRUE(columns.Bounds[0].Min.isApprox(Eigen::Vector3f{-3.5f, 15, 8}));
    EXPECT_TRUE(columns.Bounds[0].Max.isApprox(Eigen::Vector3f{3.5f, 19, 14}));
    for (size_t row = 0; row < columns.Size(); ++row) {
        const auto routed = scene.GetStaticMesh(columns.Ids[row]);
        const auto direct = columns.Get(row);
        EXPECT_EQ(&routed->LocalToWorld, &direct.LocalToWorld);
        EXPECT_EQ(&routed->Mesh, &columns.Bindings[row]);
    }
    batch.Clear();
    batch.MeshStates.push_back({{2, 1}, {}, Eigen::Matrix4f::Identity()});
    scene.Apply(batch);
    const auto rebound = scene.GetStaticMeshColumns();
    EXPECT_FALSE(rebound.Bindings[0].RenderData);
    EXPECT_FALSE(rebound.Bounds[0].ReverseCulling);
    EXPECT_TRUE(rebound.Bounds[0].Min.isZero());
    EXPECT_TRUE(rebound.Bounds[0].Max.isZero());
}

TEST(StaticMeshSceneDeathTest, RejectsInvalidBoundsAndNonAffineTransforms) {
    RenderScene scene;
    SceneUpdateBatch batch;
    batch.CreateShapes.push_back({0, 1});
    batch.MeshStates.push_back({.Id = {0, 1}});
    scene.Apply(batch);
    batch.Clear();
    batch.MeshStates.push_back({.Id = {0, 1}});
    StaticMeshRenderData invalid;
    invalid.LocalBoundsMin.x() = 1;
    batch.MeshStates[0].Mesh.RenderData = &invalid;
    EXPECT_DEATH(scene.Apply(batch), "");
#ifdef RADRAY_IS_DEBUG
    batch.Clear();
    batch.Transforms.push_back({.Id = {0, 1}});
    Eigen::Matrix4f nonAffine = Eigen::Matrix4f::Identity();
    nonAffine(3, 0) = 1;
    EXPECT_DEATH((void)AffineTransform{nonAffine}, "");
    batch.Transforms[0].LocalToWorld.Values[0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_DEATH(scene.Apply(batch), "");
#endif
}

}  // namespace
}  // namespace radray
