#include <gtest/gtest.h>

#include <cstring>
#include <optional>

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>

using namespace radray;

namespace {

AssetId MakeId(uint32_t a) {
    return AssetId{a, 0x1111, 0x4000, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
}

AssetLoadTask LoadEmptyMeshWithoutRenderData() {
    co_return AssetLoadResult::Success(make_unique<StaticMesh>());
}

class TestStaticMeshComponent final : public StaticMeshComponent {
public:
    using StaticMeshComponent::RefreshMaterialSnapshots;
};

std::optional<float> FindSnapshotFloat(
    const MaterialRenderSnapshot& snapshot,
    std::string_view name) {
    for (const auto& constant : snapshot.MaterialProperties.Constants) {
        if (constant.Name == name && constant.Bytes.size() == sizeof(float)) {
            float value = 0.0f;
            std::memcpy(&value, constant.Bytes.data(), sizeof(value));
            return value;
        }
    }
    return std::nullopt;
}

}  // namespace

TEST(StaticMeshComponentTest, DoesNotCreateRenderStateBeforeMeshHasRenderData) {
    World world;

    Actor* actor = world.SpawnActor();
    ASSERT_NE(actor, nullptr);

    auto* component = actor->AddComponent<StaticMeshComponent>();
    ASSERT_NE(component, nullptr);
    EXPECT_EQ(component->GetSceneProxy(), nullptr);

    StreamingAssetRef<StaticMesh> ref;
    component->SetStaticMesh(ref);

    EXPECT_FALSE(component->ShouldCreateRenderState());
    EXPECT_EQ(component->CreateSceneProxy(), nullptr);
    EXPECT_EQ(component->GetSceneProxy(), nullptr);
}

TEST(StaticMeshComponentTest, FailedMeshLoadLeavesRenderStateUncreated) {
    AssetManager assetManager;
    World world;

    Actor* actor = world.SpawnActor();
    ASSERT_NE(actor, nullptr);

    auto* component = actor->AddComponent<StaticMeshComponent>();
    ASSERT_NE(component, nullptr);

    StreamingAssetRef<StaticMesh> ref = assetManager.Load<StaticMesh>(AssetLoadRequest{
        .Id = MakeId(1),
        .Task = LoadEmptyMeshWithoutRenderData()});
    component->SetStaticMesh(ref);

    EXPECT_FALSE(component->ShouldCreateRenderState());
    EXPECT_EQ(component->GetSceneProxy(), nullptr);

    assetManager.Pump();
    ASSERT_TRUE(ref.IsReady());
    EXPECT_FALSE(ref->HasRenderData());

    EXPECT_FALSE(component->ShouldCreateRenderState());
    EXPECT_EQ(component->GetSceneProxy(), nullptr);
}

TEST(StaticMeshComponentTest, MaterialRevisionPublishesNewSnapshotOnNextRefresh) {
    AssetManager assets;
    auto renderMesh = make_shared<GpuMesh>();
    auto meshObject = make_unique<StaticMesh>(MeshResource{}, renderMesh);
    meshObject->SetSections({StaticMeshSection{0, 0, 3, 0, 2}});
    auto mesh = assets.AddReady<StaticMesh>(Guid::NewGuid(), std::move(meshObject));

    auto material = assets.AddReady<MaterialAsset>(
        Guid::NewGuid(),
        make_unique<MaterialAsset>());
    material->SetFloat("Value", 1.0f);

    TestStaticMeshComponent component;
    component.SetStaticMesh(mesh);
    component.SetMaterial(0, material);
    auto proxyBase = component.CreateSceneProxy();
    ASSERT_NE(proxyBase, nullptr);
    auto* proxy = static_cast<StaticMeshSceneProxy*>(proxyBase.get());
    auto first = proxy->GetSectionSnapshot(0);
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(FindSnapshotFloat(*first, "Value").has_value());
    EXPECT_FLOAT_EQ(*FindSnapshotFloat(*first, "Value"), 1.0f);

    material->SetFloat("Value", 2.0f);
    component.RefreshMaterialSnapshots(*proxy);
    auto second = proxy->GetSectionSnapshot(0);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first.get(), second.get());
    ASSERT_TRUE(FindSnapshotFloat(*second, "Value").has_value());
    EXPECT_FLOAT_EQ(*FindSnapshotFloat(*second, "Value"), 2.0f);
}
