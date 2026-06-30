#include <gtest/gtest.h>

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
