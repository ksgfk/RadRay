#include <gtest/gtest.h>

#include <limits>
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

AssetId MeshId(uint32_t value) {
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
        return Assets.AddReady<StaticMesh>(MeshId(id), MakeCpuMesh(lower, upper, std::move(sections)));
    }

    StaticMeshComponent* Add(StreamingAssetRef<StaticMesh> mesh) {
        auto* component = Owner->AddComponent<StaticMeshComponent>();
        component->SetStaticMesh(std::move(mesh));
        return component;
    }

    void Flush() {
        GameWorld.FlushRenderUpdates(Batch);
        Data.Apply(Batch);
        Batch.Clear();
    }

    AssetManager Assets;
    World GameWorld;
    Scene Data;
    SceneUpdateBatch Batch;
    Actor* Owner{GameWorld.SpawnActor()};
};

TEST_F(StaticMeshScene, CreateCombinesStateAndFinalTransform) {
    auto* component = Add(Mesh(1));
    for (int i = 0; i < 100; ++i) {
        component->SetRelativeLocation({static_cast<float>(i), 10, 20});
        component->MarkRenderStateDirty();
    }
    const PrimitiveId id = component->GetPrimitiveId();
    GameWorld.FlushRenderUpdates(Batch);
    ASSERT_EQ(Batch.CreatePrimitives.size(), 1u);
    ASSERT_EQ(Batch.MeshStates.size(), 1u);
    EXPECT_TRUE(Batch.Transforms.empty());
    EXPECT_EQ(Batch.MeshStates[0].Id, id);
    EXPECT_FLOAT_EQ(Batch.MeshStates[0].LocalToWorld(0, 3), 99);
    Data.Apply(Batch);
    Batch.Clear();
    auto view = Data.GetStaticMesh(id);
    ASSERT_TRUE(view);
    EXPECT_EQ(view->Mesh.MeshAssetId, MeshId(1));
    ASSERT_EQ(view->Mesh.Sections.size(), 1u);
    EXPECT_EQ(view->Mesh.Sections[0].IndexCount, 3u);
    ExpectBounds(*view, {-1, -2, -3}, {2, 3, 4}, component->GetWorldMatrix());
    EXPECT_FALSE(view->ReverseCulling);
    ASSERT_EQ(Data.GetStaticMeshes().size(), 1u);
    EXPECT_EQ(Data.GetStaticMeshes()[0], id);
}

TEST_F(StaticMeshScene, MovingOneObjectPreservesMeshDescriptionAndOtherObjects) {
    auto mesh = Mesh(1);
    auto* moving = Add(mesh);
    auto* stationary = Add(mesh);
    stationary->SetRelativeLocation({500, 0, 0});
    Flush();
    const auto movingId = moving->GetPrimitiveId();
    const auto stationaryId = stationary->GetPrimitiveId();
    const auto* sections = Data.GetStaticMesh(movingId)->Mesh.Sections.data();
    const auto* otherSections = Data.GetStaticMesh(stationaryId)->Mesh.Sections.data();
    const Eigen::Matrix4f otherTransform = Data.GetStaticMesh(stationaryId)->LocalToWorld;
    const Eigen::Vector3f otherBounds = Data.GetStaticMesh(stationaryId)->WorldBoundsMin;
    for (int i = 0; i < 100; ++i) moving->SetRelativeLocation({static_cast<float>(i), 0, 0});
    GameWorld.FlushRenderUpdates(Batch);
    EXPECT_TRUE(Batch.MeshStates.empty());
    ASSERT_EQ(Batch.Transforms.size(), 1u);
    EXPECT_EQ(Batch.Transforms[0].Id, movingId);
    Data.Apply(Batch);
    Batch.Clear();
    EXPECT_EQ(Data.GetStaticMesh(movingId)->Mesh.Sections.data(), sections);
    EXPECT_EQ(Data.GetStaticMesh(stationaryId)->Mesh.Sections.data(), otherSections);
    EXPECT_TRUE(Data.GetStaticMesh(stationaryId)->LocalToWorld.isApprox(otherTransform));
    EXPECT_TRUE(Data.GetStaticMesh(stationaryId)->WorldBoundsMin.isApprox(otherBounds));
    ExpectBounds(*Data.GetStaticMesh(movingId), {-1, -2, -3}, {2, 3, 4}, moving->GetWorldMatrix());
    GameWorld.FlushRenderUpdates(Batch);
    EXPECT_TRUE(Batch.Empty());
}

TEST_F(StaticMeshScene, ReplacementUsesNewBoundsAndFinalTransformWithoutChangingId) {
    auto* component = Add(Mesh(1));
    Flush();
    const auto id = component->GetPrimitiveId();
    component->SetStaticMesh(Mesh(2, {-10, -20, -30}, {30, 40, 50}, {{0, 0, 1, 0, 0}, {0, 1, 2, 1, 2}}));
    component->SetRelativeScale({-2, 3, 4});
    component->SetRelativeLocation({30, 50, 70});
    GameWorld.FlushRenderUpdates(Batch);
    EXPECT_TRUE(Batch.CreatePrimitives.empty());
    EXPECT_TRUE(Batch.Transforms.empty());
    ASSERT_EQ(Batch.MeshStates.size(), 1u);
    EXPECT_EQ(Batch.MeshStates[0].Id, id);
    Data.Apply(Batch);
    Batch.Clear();
    auto view = Data.GetStaticMesh(id);
    ASSERT_TRUE(view);
    EXPECT_EQ(view->Mesh.MeshAssetId, MeshId(2));
    ASSERT_EQ(view->Mesh.Sections.size(), 2u);
    EXPECT_EQ(view->Mesh.Sections[1].FirstIndex, 1u);
    ExpectBounds(*view, {-10, -20, -30}, {30, 40, 50}, component->GetWorldMatrix());
    EXPECT_TRUE(view->ReverseCulling);
    EXPECT_EQ(component->GetPrimitiveId(), id);
}

TEST_F(StaticMeshScene, ParentRotationNonuniformScaleAndReflectionsUpdateBoundsAndWinding) {
    auto* parent = Owner->AddComponent<SceneComponent>();
    parent->SetRelativeScale({2, 3, 4});
    parent->SetRelativeRotation(Eigen::Quaternionf{Eigen::AngleAxisf{0.7f, Eigen::Vector3f::UnitY()}});
    auto* component = Add(Mesh(1));
    component->AttachTo(parent);
    component->SetRelativeRotation(Eigen::Quaternionf{Eigen::AngleAxisf{0.3f, Eigen::Vector3f::UnitZ()}});
    component->SetRelativeLocation({3, 4, 5});
    Flush();
    const auto id = component->GetPrimitiveId();
    for (const Eigen::Vector3f scale : {Eigen::Vector3f{-1, 2, 3}, Eigen::Vector3f{-1, -2, 3}, Eigen::Vector3f{0, 2, 3}, Eigen::Vector3f{-1e-20f, 1e-20f, 1e-20f}}) {
        component->SetRelativeScale(scale);
        GameWorld.FlushRenderUpdates(Batch);
        EXPECT_TRUE(Batch.MeshStates.empty());
        ASSERT_EQ(Batch.Transforms.size(), 1u);
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
    const auto id = component->GetPrimitiveId();
    component->SetStaticMesh({});
    component->SetRelativeLocation({5, 6, 7});
    Flush();
    auto view = Data.GetStaticMesh(id);
    ASSERT_TRUE(view);
    EXPECT_TRUE(view->Mesh.MeshAssetId.IsEmpty());
    EXPECT_TRUE(view->Mesh.Sections.empty());
    EXPECT_FLOAT_EQ(view->LocalToWorld(0, 3), 5);
    auto invalid = Assets.AddReady<StaticMesh>(MeshId(2), make_unique<CpuMesh>(MeshResource{}, vector<StaticMeshSection>{},
                                                                               Eigen::Vector3f::Zero(), Eigen::Vector3f::Zero(), GpuMesh{}));
    component->SetStaticMesh(invalid);
    Flush();
    EXPECT_TRUE(Data.GetStaticMesh(id)->Mesh.Sections.empty());
    EXPECT_EQ(Data.GetStaticMesh(id)->Mesh.MeshAssetId, MeshId(2));
    component->SetStaticMesh(Mesh(3));
    Flush();
    EXPECT_EQ(Data.GetStaticMesh(id)->Mesh.Sections.size(), 1u);
    EXPECT_EQ(component->GetPrimitiveId(), id);
}

TEST_F(StaticMeshScene, MeshWithoutSectionsProducesFullPrimitiveDescription) {
    auto* component = Add(Mesh(1, {-1, -1, -1}, {1, 1, 1}, {}));
    Flush();
    const auto& sections = Data.GetStaticMesh(component->GetPrimitiveId())->Mesh.Sections;
    ASSERT_EQ(sections.size(), 1u);
    EXPECT_EQ(sections[0].PrimitiveIndex, 0u);
    EXPECT_EQ(sections[0].FirstIndex, 0u);
    EXPECT_EQ(sections[0].IndexCount, 3u);
    EXPECT_EQ(sections[0].MaxVertexIndex, 2u);
}

TEST_F(StaticMeshScene, ReadyAutomaticallyPublishesTheLatestTransform) {
    auto* component = Add(Mesh(1));
    Flush();
    auto loading = Assets.Load({.Id = MeshId(2), .Task = []() -> task<AssetLoadResult> {
                                    co_return AssetLoadResult::Success(MakeCpuMesh({-2, -3, -4}, {3, 4, 5}, {{0, 0, 3, 0, 2}}));
                                }()})
                       .CastTo<StaticMesh>();
    ASSERT_FALSE(loading.IsReady());
    const auto id = component->GetPrimitiveId();
    component->SetStaticMesh(loading);
    component->SetRelativeLocation({20, 0, 0});
    Flush();
    EXPECT_TRUE(Data.GetStaticMesh(id)->Mesh.Sections.empty());
    EXPECT_EQ(Data.GetStaticMesh(id)->Mesh.MeshAssetId, MeshId(2));
    Assets.Pump();
    ASSERT_TRUE(loading.IsReady());
    component->SetRelativeLocation({50, 0, 0});
    Flush();
    ASSERT_EQ(Data.GetStaticMesh(id)->Mesh.Sections.size(), 1u);
    ExpectBounds(*Data.GetStaticMesh(id), {-2, -3, -4}, {3, 4, 5}, component->GetWorldMatrix());
}

TEST_F(StaticMeshScene, DestroyingUnsentMeshLeavesNoTypedPayload) {
    auto* component = Add(Mesh(1));
    component->SetRelativeLocation({100, 0, 0});
    Owner->RemoveComponent(component);
    GameWorld.FlushRenderUpdates(Batch);
    EXPECT_TRUE(Batch.Empty());
    Data.Apply(Batch);
    EXPECT_TRUE(Data.GetStaticMeshes().empty());
}

TEST_F(StaticMeshScene, FlightRetainsAssetAfterSourceDiesBeforeApplyOnAnotherThread) {
    auto* component = Add(Mesh(1));
    const auto id = component->GetPrimitiveId();
    SceneUpdateBatch create;
    GameWorld.FlushRenderUpdates(create);
    vector<StreamingAssetRef<StaticMesh>> refs;
    GameWorld.RetainRenderAssets(&Assets, refs);
    Owner->RemoveComponent(component);
    Assets.Pump();
    EXPECT_EQ(Assets.GetAssetCount(), 1u);
    SceneUpdateBatch remove;
    GameWorld.FlushRenderUpdates(remove);
    EXPECT_TRUE(remove.MeshStates.empty());
    EXPECT_TRUE(remove.Transforms.empty());
    std::thread render([&]() {
        Data.Apply(create);
        auto view = Data.GetStaticMesh(id);
        ASSERT_TRUE(view);
        EXPECT_EQ(view->Mesh.MeshAssetId, MeshId(1));
        ASSERT_EQ(view->Mesh.Sections.size(), 1u);
        EXPECT_EQ(view->Mesh.Sections[0].IndexCount, 3u);
        ASSERT_TRUE(view->Mesh.RenderMesh);
        EXPECT_TRUE(view->Mesh.RenderMesh->Draws.empty());
        Data.Apply(remove);
        EXPECT_FALSE(Data.GetStaticMesh(id));
        EXPECT_TRUE(Data.GetStaticMeshes().empty());
    });
    render.join();
    refs.clear();
    Assets.Pump();
    EXPECT_EQ(Assets.GetAssetCount(), 0u);
}

TEST_F(StaticMeshScene, RemovalRepairsDenseListAndReuseCannotExposeOldMesh) {
    vector<StaticMeshComponent*> components;
    for (int i = 0; i < 3; ++i) components.push_back(Add(Mesh(1)));
    Flush();
    const auto oldId = components[1]->GetPrimitiveId();
    Owner->RemoveComponent(components[1]);
    auto* replacement = Add(Mesh(2));
    const auto newId = replacement->GetPrimitiveId();
    EXPECT_EQ(oldId.Index, newId.Index);
    Flush();
    EXPECT_FALSE(Data.GetStaticMesh(oldId));
    ASSERT_TRUE(Data.GetStaticMesh(newId));
    EXPECT_EQ(Data.GetStaticMesh(newId)->Mesh.MeshAssetId, MeshId(2));
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
    const auto id = component->GetPrimitiveId();
    const auto* description = &Data.GetStaticMesh(id)->Mesh;
    const auto* sections = description->Sections.data();
    for (int frame = 0; frame < 8; ++frame) {
        GameWorld.FlushRenderUpdates(Batch);
        EXPECT_TRUE(Batch.Empty());
        Data.Apply(Batch);
        for (int viewIndex = 0; viewIndex < 3; ++viewIndex) {
            const auto view = Data.GetStaticMesh(id);
            ASSERT_TRUE(view);
            EXPECT_EQ(&view->Mesh, description);
            EXPECT_EQ(view->Mesh.Sections.data(), sections);
        }
    }
}

TEST_F(StaticMeshScene, ReusedFlightsNeverRestoreAnOlderTransform) {
    Application app;
    for (uint32_t count : {1u, 2u, 3u}) {
        World world;
        RenderSystem render{&app, count};
        render.SetAssetManager(&Assets);
        auto* component = world.SpawnActor()->AddComponent<StaticMeshComponent>();
        component->SetStaticMesh(Mesh(1));
        const auto id = component->GetPrimitiveId();
        for (uint32_t frame = 0; frame < count * 4; ++frame) {
            if (frame == 1) component->SetRelativeLocation({15, 0, 0});
            const uint32_t flight = frame % count;
            render.PrepareFrameGT(world, {.FlightIndex = flight});
            render.ConsumeRenderUpdates(flight);
            const auto view = render.GetScene().GetStaticMesh(id);
            ASSERT_TRUE(view);
            EXPECT_FLOAT_EQ(view->LocalToWorld(0, 3), frame == 0 ? 0 : 15);
            render.OnFlightCompletedGT({.FlightIndex = flight, .GpuWorkCompleted = false});
        }
    }
}

TEST_F(StaticMeshScene, TransformBatchReusesStorageAfterWarmup) {
    auto* component = Add(Mesh(1));
    Flush();
    component->SetRelativeLocation({1, 0, 0});
    GameWorld.FlushRenderUpdates(Batch);
    const auto* storage = Batch.Transforms.data();
    const auto capacity = Batch.Transforms.capacity();
    Data.Apply(Batch);
    Batch.Clear();
    for (int frame = 0; frame < 8; ++frame) {
        component->SetRelativeLocation({static_cast<float>(frame), 0, 0});
        GameWorld.FlushRenderUpdates(Batch);
        EXPECT_TRUE(Batch.MeshStates.empty());
        EXPECT_EQ(Batch.Transforms.data(), storage);
        EXPECT_EQ(Batch.Transforms.capacity(), capacity);
        Data.Apply(Batch);
        Batch.Clear();
    }
}

TEST(StaticMeshSceneDeathTest, RejectsStaleStateTransformAndTransformWithoutMesh) {
    Scene scene;
    SceneUpdateBatch batch;
    batch.CreatePrimitives.push_back({0, 1});
    batch.MeshStates.push_back({.Id = {0, 1}});
    scene.Apply(batch);
    batch.Clear();
    batch.RemovePrimitives.push_back({0, 1});
    batch.CreatePrimitives.push_back({0, 2});
    batch.MeshStates.push_back({.Id = {0, 2}});
    scene.Apply(batch);
    batch.Clear();
    batch.MeshStates.push_back({.Id = {0, 1}});
    EXPECT_DEATH(scene.Apply(batch), "");
    batch.Clear();
    batch.Transforms.push_back({.Id = {0, 1}});
    EXPECT_DEATH(scene.Apply(batch), "");
    batch.Clear();
    batch.CreatePrimitives.push_back({1, 1});
    scene.Apply(batch);
    batch.Clear();
    batch.Transforms.push_back({.Id = {1, 1}});
    EXPECT_DEATH(scene.Apply(batch), "");
}

TEST(StaticMeshSceneDeathTest, RejectsInvalidBoundsAndNonAffineTransforms) {
    Scene scene;
    SceneUpdateBatch batch;
    batch.CreatePrimitives.push_back({0, 1});
    batch.MeshStates.push_back({.Id = {0, 1}});
    scene.Apply(batch);
    batch.Clear();
    batch.MeshStates.push_back({.Id = {0, 1}});
    batch.MeshStates[0].Mesh.LocalBoundsMin.x() = 1;
    EXPECT_DEATH(scene.Apply(batch), "");
    batch.Clear();
    batch.Transforms.push_back({.Id = {0, 1}});
    batch.Transforms[0].LocalToWorld(3, 0) = 1;
    EXPECT_DEATH(scene.Apply(batch), "");
    batch.Transforms[0].LocalToWorld(3, 0) = std::numeric_limits<float>::quiet_NaN();
    EXPECT_DEATH(scene.Apply(batch), "");
}

TEST(StaticMeshSceneDeathTest, FlushRejectsExistingTypedPayload) {
    World world;
    SceneUpdateBatch batch;
    batch.MeshStates.push_back({});
    EXPECT_DEATH(world.FlushRenderUpdates(batch), "");
    batch.Clear();
    batch.Transforms.push_back({});
    EXPECT_DEATH(world.FlushRenderUpdates(batch), "");
}

}  // namespace
}  // namespace radray
