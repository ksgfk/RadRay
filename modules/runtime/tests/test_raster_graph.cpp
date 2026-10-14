#include <gtest/gtest.h>
#include <radray/runtime/render_framework/raster_graph.h>
#include <radray/runtime/render_framework/raster_view.h>
#include <radray/runtime/render_framework/raster_scene.h>
#include <radray/runtime/render_framework/scene_draw.h>

namespace radray::test {
TEST(RasterGraph, CompilationSchedulesHazardsWithoutRecordingCallbacks) {
    RasterGraph graph;
    const auto a = graph.AddResource({}), b = graph.AddResource({});
    uint32_t recorded = 0;
    auto body = [&](render::CommandBuffer&) { ++recorded; return true; };
    graph.AddNode({RasterNodeKind::Clear, {{a, RasterAccess::Write, render::TextureState::RenderTarget}}, {}, body});
    graph.AddNode({RasterNodeKind::Fullscreen, {{a, RasterAccess::Read, render::TextureState::ShaderRead}, {b, RasterAccess::Write, render::TextureState::RenderTarget}}, {}, body});
    graph.AddNode({RasterNodeKind::Fullscreen, {{b, RasterAccess::Read, render::TextureState::ShaderRead}, {a, RasterAccess::Write, render::TextureState::RenderTarget}}, {}, body});
    graph.AddNode({RasterNodeKind::Raster, {{a, RasterAccess::ReadWrite, render::TextureState::RenderTarget}}, {}, body});
    auto result = graph.Compile();
    ASSERT_TRUE(result);
    ASSERT_EQ(result->Steps.size(), 4u);
    EXPECT_EQ(recorded, 0u);
    EXPECT_EQ(result->Steps[3].Node, 3u);
    EXPECT_EQ(result->Steps[3].Barriers.size(), 0u);
    ASSERT_EQ(result->ExitBarriers.size(), 1u);
    EXPECT_EQ(result->ExitBarriers[0].After, render::TextureState::ShaderRead);
}
TEST(RasterGraph, RejectsCyclesFeedbackAndUninitializedReads) {
    RasterGraph graph;
    auto a = graph.AddResource({});
    graph.AddNode({RasterNodeKind::Raster, {{a, RasterAccess::Read, render::TextureState::ShaderRead}}});
    EXPECT_FALSE(graph.Compile());
    RasterGraph feedback;
    a = feedback.AddResource({nullptr, render::TextureState::ShaderRead, render::TextureState::ShaderRead, true});
    feedback.AddNode({RasterNodeKind::Fullscreen, {{a, RasterAccess::Read, render::TextureState::ShaderRead}, {a, RasterAccess::Write, render::TextureState::RenderTarget}}});
    EXPECT_FALSE(feedback.Compile());
    RasterGraph cycle;
    cycle.AddNode({RasterNodeKind::Raster, {}, {1}});
    cycle.AddNode({RasterNodeKind::Raster, {}, {0}});
    EXPECT_FALSE(cycle.Compile());
}
TEST(RasterView, EvaluatedProjectionPreservesReverseDepthAndConvertsBackendViewportOnce) {
    RasterView view{Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity(), {10, 20, 60, 40, 0, 1}, {10, 20, 60, 40}, RasterDepthConvention::ReverseZeroToOne};
    auto d3 = ResolveRasterView(view, 100, 80, render::RenderBackend::D3D12);
    auto vk = ResolveRasterView(view, 100, 80, render::RenderBackend::Vulkan);
    ASSERT_TRUE(d3);
    ASSERT_TRUE(vk);
    EXPECT_TRUE(d3->ReverseDepth);
    EXPECT_TRUE(d3->ViewProjection.isApprox(vk->ViewProjection));
    EXPECT_FLOAT_EQ(vk->Viewport.Y, 60);
    EXPECT_FLOAT_EQ(vk->Viewport.Height, -40);
    view.Depth = RasterDepthConvention::MinusOneToOne;
    auto gl = ResolveRasterView(view, 100, 80, render::RenderBackend::D3D12);
    ASSERT_TRUE(gl);
    Eigen::Vector4f sampleNear{0, 0, -1, 1};
    EXPECT_FLOAT_EQ((gl->Projection * sampleNear).z(), 0);
    view.Scissor.Width = 101;
    EXPECT_FALSE(ResolveRasterView(view, 100, 80, render::RenderBackend::D3D12));
}
TEST(RasterScene, DoubleBoundsSplitPositionAndIndivisibleDomainsAreIndependentOfVertexLayout) {
    const Eigen::Vector3d camera{30000000.125, -30000000.25, 0};
    auto split = SplitRasterPosition(camera);
    ASSERT_TRUE(split);
    EXPECT_DOUBLE_EQ(double(split->High[0]) + split->Low[0], camera.x());
    EXPECT_TRUE(RasterBoundsVisible(camera + Eigen::Vector3d{-.25, -.25, .1}, camera + Eigen::Vector3d{.25, .25, .9}, camera, Eigen::Matrix4f::Identity()));
    EXPECT_FALSE(RasterBoundsVisible(camera + Eigen::Vector3d{2, 0, 0}, camera + Eigen::Vector3d{3, 1, 1}, camera, Eigen::Matrix4f::Identity()));
    const uint32_t source[]{1, 2, 3, 4, 5, 6, 7, 8, 9};
    array<uint32_t, 9> target{};
    vector<RasterSortGroup> groups{{0, 3, {0, 0, 1}, 0}, {3, 3, {0, 0, 8}, 0}, {6, 3, {0, 0, 20}, 1}};
    ASSERT_TRUE(SortRasterIndices(source, groups, Eigen::Matrix4d::Identity(), Eigen::Vector3d::Zero(), false, target));
    EXPECT_EQ(target[0], 4u);
    EXPECT_EQ(target[3], 1u);
    EXPECT_EQ(target[6], 7u);
    ASSERT_TRUE(SortRasterIndices(source, groups, Eigen::Matrix4d::Identity(), Eigen::Vector3d::Zero(), true, target));
    EXPECT_EQ(target[0], 1u);
    groups[1].FirstIndex = 2;
    EXPECT_FALSE(SortRasterIndices(source, groups, Eigen::Matrix4d::Identity(), Eigen::Vector3d::Zero(), false, target));
}
TEST(RasterScene, PersistentRecordsRefreshMaterialGeometryAndGenerationWhileParametersAndOrderStayIndependent) {
    RenderScene scene;
    SceneUpdateBatch update;
    StaticMeshRenderData first, second;
    first.Mesh.Draws.resize(1);
    first.Sections.emplace_back(0, 0, 3, 0, 2);
    second.Mesh.Draws.resize(2);
    second.Sections.emplace_back(1, 3, 6, 0, 3);
    MaterialRenderData red, blue;
    ShapeId id{4, 0};
    update.CreateShapes = {id};
    update.MeshStates = {{.Id = id, .Mesh = {{}, &first}}};
    update.Materials = {{id, {{{}, &red}}}};
    scene.Apply(update);
    auto records = scene.GetDrawPrimitives(id);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].Binding.Data.Get(), &red);
    const auto before = *scene.GetDrawRevision(id);
    update.Clear();
    update.Materials = {{id, {{{}, &blue}}}};
    scene.Apply(update);
    records = scene.GetDrawPrimitives(id);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].Binding.Data.Get(), &blue);
    EXPECT_EQ(scene.GetDrawRevision(id)->Geometry, before.Geometry);
    update.Clear();
    update.DrawStates = {{id, {2, 3, 7}}};
    InstanceParameterLayout layout{"Tint", 16, {{"Color", 0, 16, 0, 1, 1, 4}}};
    update.Parameters = {{id, layout, vector<byte>(16)}};
    scene.Apply(update);
    EXPECT_EQ(scene.GetDrawRevision(id)->Geometry, before.Geometry);
    EXPECT_EQ(scene.GetDrawState(id)->Order, 7u);
    EXPECT_EQ(scene.GetDrawPrimitives(id)[0].Section, &first.Sections[0]);
    update.Clear();
    update.MeshStates = {{.Id = id, .Mesh = {{}, &second}}};
    scene.Apply(update);
    records = scene.GetDrawPrimitives(id);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].Draw, &second.Mesh.Draws[1]);
    update.Clear();
    update.RemoveShapes = {id};
    auto replacement = id;
    replacement.Generation = 1;
    update.CreateShapes = {replacement};
    update.MeshStates = {{.Id = replacement, .Mesh = {{}, &first}}};
    scene.Apply(update);
    EXPECT_TRUE(scene.GetDrawPrimitives(id).empty());
    EXPECT_EQ(scene.GetDrawPrimitives(replacement)[0].Section, &first.Sections[0]);
}
}  // namespace radray::test
