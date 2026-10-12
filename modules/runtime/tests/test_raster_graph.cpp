#include <gtest/gtest.h>
#include <radray/runtime/render_framework/raster_graph.h>
#include <radray/runtime/render_framework/raster_view.h>

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
    ASSERT_TRUE(result); ASSERT_EQ(result->Steps.size(), 4u); EXPECT_EQ(recorded, 0u);
    EXPECT_EQ(result->Steps[3].Node, 3u); EXPECT_EQ(result->Steps[3].Barriers.size(), 0u);
    ASSERT_EQ(result->ExitBarriers.size(), 1u); EXPECT_EQ(result->ExitBarriers[0].After, render::TextureState::ShaderRead);
}
TEST(RasterGraph, RejectsCyclesFeedbackAndUninitializedReads) {
    RasterGraph graph; auto a = graph.AddResource({});
    graph.AddNode({RasterNodeKind::Raster, {{a, RasterAccess::Read, render::TextureState::ShaderRead}}});
    EXPECT_FALSE(graph.Compile());
    RasterGraph feedback; a = feedback.AddResource({nullptr, render::TextureState::ShaderRead, render::TextureState::ShaderRead, true});
    feedback.AddNode({RasterNodeKind::Fullscreen, {{a, RasterAccess::Read, render::TextureState::ShaderRead}, {a, RasterAccess::Write, render::TextureState::RenderTarget}}});
    EXPECT_FALSE(feedback.Compile());
    RasterGraph cycle;
    cycle.AddNode({RasterNodeKind::Raster, {}, {1}}); cycle.AddNode({RasterNodeKind::Raster, {}, {0}});
    EXPECT_FALSE(cycle.Compile());
}
TEST(RasterView, EvaluatedProjectionPreservesReverseDepthAndConvertsBackendViewportOnce) {
    RasterView view{Eigen::Matrix4f::Identity(), Eigen::Matrix4f::Identity(), {10, 20, 60, 40, 0, 1}, {10, 20, 60, 40}, RasterDepthConvention::ReverseZeroToOne};
    auto d3 = ResolveRasterView(view, 100, 80, render::RenderBackend::D3D12);
    auto vk = ResolveRasterView(view, 100, 80, render::RenderBackend::Vulkan);
    ASSERT_TRUE(d3); ASSERT_TRUE(vk); EXPECT_TRUE(d3->ReverseDepth);
    EXPECT_TRUE(d3->ViewProjection.isApprox(vk->ViewProjection)); EXPECT_FLOAT_EQ(vk->Viewport.Y, 60); EXPECT_FLOAT_EQ(vk->Viewport.Height, -40);
    view.Depth = RasterDepthConvention::MinusOneToOne;
    auto gl = ResolveRasterView(view, 100, 80, render::RenderBackend::D3D12); ASSERT_TRUE(gl);
    Eigen::Vector4f sampleNear{0, 0, -1, 1}; EXPECT_FLOAT_EQ((gl->Projection * sampleNear).z(), 0);
    view.Scissor.Width = 101; EXPECT_FALSE(ResolveRasterView(view, 100, 80, render::RenderBackend::D3D12));
}
}  // namespace radray::test
