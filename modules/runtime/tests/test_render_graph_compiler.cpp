#include <radray/runtime/render_framework/render_graph_compiler.h>

#include <algorithm>
#include <gtest/gtest.h>

namespace radray {
namespace {
constexpr uint32_t None = RgInvalidIndex;

TEST(RenderGraphCompilerTest, SchedulesAProducerDeclaredAfterItsConsumer) {
    const vector<RgResourceVersionNode> versions{{0, 0, 0, None, None, false}, {0, 0, 1, 2, 0, true}};
    const vector<RgExecutionNode> passes{{{1}, {}, true}, {{}, {}, true}, {{}, {1}, false}};
    const auto graph = CompileRenderGraph(1, versions, passes, {});
    ASSERT_TRUE(graph.IsValid());
    EXPECT_EQ(graph.ExecutionOrder, (vector<uint32_t>{1, 2, 0}));
    EXPECT_EQ(graph.Passes[0].DataDependencies, (vector<uint32_t>{2}));
    EXPECT_EQ(graph.Lifetimes[0].FirstUse, 1);
    EXPECT_EQ(graph.Lifetimes[0].LastUse, 2);
}

TEST(RenderGraphCompilerTest, DiscardingOverwriteDoesNotRetainItsStoragePredecessor) {
    const vector<RgResourceVersionNode> versions{{0, 0, 0, None, None, false}, {0, 0, 1, 0, 0, true}, {0, 0, 2, 1, 1, true}};
    const vector<RgExecutionNode> passes{{{}, {1}, false}, {{}, {2}, false}};
    const uint32_t root = 2;
    auto graph = CompileRenderGraph(1, versions, passes, std::span{&root, 1});
    ASSERT_TRUE(graph.IsValid());
    EXPECT_EQ(graph.ExecutionOrder, (vector<uint32_t>{1}));
    EXPECT_FALSE(graph.Passes[0].Live);
    graph = CompileRenderGraph(1, versions, passes, std::span{&root, 1}, {.CullPasses = false});
    ASSERT_TRUE(graph.IsValid());
    EXPECT_EQ(graph.ExecutionOrder, (vector<uint32_t>{0, 1}));
}

TEST(RenderGraphCompilerTest, OldVersionReadersRunBeforeDestructiveSuccessors) {
    const vector<RgResourceVersionNode> versions{{0, 0, 0, None, None, true}, {0, 0, 1, 0, 0, true}};
    const vector<RgExecutionNode> passes{{{}, {1}, true}, {{0}, {}, true}};
    const auto graph = CompileRenderGraph(1, versions, passes, {});
    ASSERT_TRUE(graph.IsValid());
    EXPECT_EQ(graph.ExecutionOrder, (vector<uint32_t>{1, 0}));
    EXPECT_EQ(graph.Passes[0].HazardDependencies, (vector<uint32_t>{1}));
}

TEST(RenderGraphCompilerTest, RejectsReadingOldAndNewContentsAfterAnInPlaceOverwrite) {
    const vector<RgResourceVersionNode> versions{{0, 0, 0, None, None, true}, {0, 0, 1, 0, 0, true}};
    const vector<RgExecutionNode> passes{{{}, {1}, false}, {{0, 1}, {}, true}};
    const auto graph = CompileRenderGraph(1, versions, passes, {});
    ASSERT_FALSE(graph.IsValid());
    EXPECT_TRUE(std::any_of(graph.Diagnostics.begin(), graph.Diagnostics.end(), [](const auto& d) { return d.Code == "DependencyCycle"; }));
}

TEST(RenderGraphCompilerTest, PreservedContentsRetainTheirProducerAndRangesStayIndependent) {
    const vector<RgResourceVersionNode> versions{
        {0, 0, 0, None, None, false}, {0, 1, 0, None, None, false}, {0, 0, 1, 0, 0, true}, {0, 1, 1, 1, 1, true}, {0, 0, 2, 2, 2, true}};
    const vector<RgExecutionNode> passes{{{}, {2}, false}, {{}, {3}, false}, {{2}, {4}, false}};
    const uint32_t root = 4;
    const auto graph = CompileRenderGraph(1, versions, passes, std::span{&root, 1});
    ASSERT_TRUE(graph.IsValid());
    EXPECT_EQ(graph.ExecutionOrder, (vector<uint32_t>{0, 2}));
    EXPECT_FALSE(graph.Passes[1].Live);
}

TEST(RenderGraphCompilerTest, ValidatesDeadReadsAndRejectsUninitializedExports) {
    const vector<RgResourceVersionNode> versions{{0, 0, 0, None, None, false}};
    const vector<RgExecutionNode> passes{{{0}, {}, false}};
    const auto graph = CompileRenderGraph(1, versions, passes, {});
    ASSERT_FALSE(graph.IsValid());
    EXPECT_EQ(graph.Diagnostics.front().Code, "UninitializedRead");
    const uint32_t root = 0;
    const auto exported = CompileRenderGraph(1, versions, {}, std::span{&root, 1});
    ASSERT_FALSE(exported.IsValid());
    EXPECT_EQ(exported.Diagnostics.front().Code, "UninitializedExport");
}

TEST(RenderGraphCompilerTest, MalformedIndicesAndVersionChainsAreDiagnostics) {
    const vector<RgResourceVersionNode> versions{{0, 0, 1, 0, 0, true}};
    const vector<RgExecutionNode> passes{{{}, {0}, true}};
    EXPECT_FALSE(CompileRenderGraph(1, versions, passes, {}).IsValid());
    EXPECT_FALSE(CompileRenderGraph(0, versions, passes, {}).IsValid());
}
}  // namespace
}  // namespace radray
