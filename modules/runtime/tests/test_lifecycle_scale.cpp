#include "scene_test_support.h"
#include <gtest/gtest.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>

namespace radray {
namespace {
unique_ptr<StaticMesh> MakeTestMesh() {
    const array<float, 9> positions{0, 0, 0, 1, 0, 0, 0, 1, 0};
    const array<uint32_t, 3> indices{0, 1, 2};
    MeshResource mesh;
    mesh.Bins.emplace_back(std::as_bytes(std::span{positions}));
    mesh.Bins.emplace_back(std::as_bytes(std::span{indices}));
    MeshPrimitive primitive;
    primitive.VertexCount = 3;
    primitive.VertexBuffers.push_back({"POSITION", 0, 0, VertexDataType::FLOAT, 3, 0, 12});
    primitive.IndexBuffer = {1, 3, 0, 4};
    mesh.Primitives.push_back(std::move(primitive));
    return make_unique<StaticMesh>(std::move(mesh), vector<StaticMeshSection>{}, Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), GpuMesh{});
}

TEST(LifecycleScale, SharedMeshViewsAndTransformCapture) {
    for (uint32_t count : {10000u, 100000u}) {
        Application app;
        AssetManager assets;
        RenderSystem renderer{&app, 1};
        test::ScopedWorld world;
        const auto scene = test::ConnectWorld(world, renderer);
        const auto mesh = assets.AddReady<StaticMesh>(AssetId{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, MakeTestMesh());
        ASSERT_TRUE(mesh->IsValid());
        ASSERT_EQ(mesh->GetSections().size(), 1u);
        EXPECT_EQ(mesh->GetSections()[0].IndexCount, 3u);
        const auto* sections = mesh->GetSections().data();
        for (uint32_t i = 0; i < count; ++i) world.SpawnActor()->AddComponent<StaticMeshComponent>()->SetStaticMesh(mesh);
        test::PrepareScene(world, renderer, 0);
        EXPECT_EQ(test::SceneBatch(renderer, scene, 0).MeshStates.size(), count);
        for (const auto& state : test::SceneBatch(renderer, scene, 0).MeshStates) {
            EXPECT_EQ(state.Mesh.GetSections().data(), sections);
            EXPECT_EQ(state.Mesh.GetRenderMesh().Get(), &mesh->GetRenderMesh());
        }
        test::ConsumeFrame(renderer, 0);
        test::CompleteFrame(renderer, 0);
        auto component = world.GetActors()[0]->FindComponent<StaticMeshComponent>();
        for (int i = 0; i < 100; ++i) component->SetRelativeLocation({float(i), 0, 0});
        test::PrepareScene(world, renderer, 0);
        ASSERT_EQ(test::SceneBatch(renderer, scene, 0).LocalTransforms.size(), 1u);
        EXPECT_TRUE(test::SceneBatch(renderer, scene, 0).MeshStates.empty());
        EXPECT_EQ(mesh->GetSections().data(), sections);
        EXPECT_FLOAT_EQ(test::SceneBatch(renderer, scene, 0).LocalTransforms[0].Local.Translation[0], 99);
    }
}
}  // namespace
}  // namespace radray
