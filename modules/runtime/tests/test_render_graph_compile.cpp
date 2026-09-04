#include "graph_compile_device.h"
#include <radray/runtime/render_framework/render_graph.h>
#include <chrono>
#include <gtest/gtest.h>

namespace radray {
namespace {
struct EmptyPass {};
void EmptyRaster(const EmptyPass&, RenderGraphRasterContext&) {}
void EmptyCompute(const EmptyPass&, RenderGraphComputeContext&) {}
render::TextureDescriptor GraphColor(uint32_t mips = 1) {
    return {render::TextureDimension::Dim2D, 16, 16, 1, mips, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource, {}};
}
RgPassHandle Clear(RenderGraph& graph, RgTextureHandle texture, std::string_view name = "clear", render::LoadAction load = render::LoadAction::Clear,
                   render::StoreAction store = render::StoreAction::Store, bool root = false, uint32_t mip = 0) {
    return graph.AddRasterPass<EmptyPass>(name, [=](EmptyPass&, RenderGraphRasterBuilder& builder) {
        RgColorAttachmentDesc desc;
        desc.Load = load; desc.Store = store; desc.View.Range = {0, 1, mip, 1}; desc.Clear = {.25f, .5f, .75f, 1};
        builder.SetColorAttachment(0, texture, desc);
        if (root) builder.SetSideEffect(); }, EmptyRaster);
}
class RenderGraphCompileTest : public testing::Test {
protected:
    void SetUp() override {
        Registry = make_unique<render::RenderPassRegistry>(&Device);
        Pool = make_unique<RenderResourcePool>(Device, *Registry);
        Pool->BeginFlight(1);
    }
    void TearDown() override { EXPECT_EQ(Device.NativeCreates, 0u); }
    RenderGraph MakeGraph(std::string_view name = "compile") { return RenderGraph{Device, *Pool, *Registry, name}; }
    test::GraphCompileDevice Device;
    unique_ptr<render::RenderPassRegistry> Registry;
    unique_ptr<RenderResourcePool> Pool;
};

TEST_F(RenderGraphCompileTest, ContentVersionsCullDiscardingOverwriteButPreserveLoad) {
    for (const auto load : {render::LoadAction::Clear, render::LoadAction::Load}) {
        auto graph = MakeGraph();
        auto color = graph.CreateTexture(GraphColor(), "color");
        Clear(graph, color, "old");
        Clear(graph, color, "final", load, render::StoreAction::Store, true);
        auto unused = graph.CreateTexture(GraphColor(), "unused");
        Clear(graph, unused, "unused");
        ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
        const auto& report = graph.GetReport();
        EXPECT_EQ(report.Passes[0].Live, load == render::LoadAction::Load);
        EXPECT_TRUE(report.Passes[1].Live);
        EXPECT_FALSE(report.Passes[2].Live);
        EXPECT_EQ(report.Resources[1].FirstUse, -1);
        EXPECT_EQ(Pool->GetStats().Created, 0u);
    }
}

TEST_F(RenderGraphCompileTest, RejectsUninitializedReadLoadDiscardAndFeedbackBeforeAllocation) {
    for (uint32_t scenario = 0; scenario < 5; ++scenario) {
        auto graph = MakeGraph();
        auto color = graph.CreateTexture(GraphColor(2), "uninitialized");
        auto output = graph.CreateTexture(GraphColor(), "output");
        if (scenario == 0)
            Clear(graph, color, "load", render::LoadAction::Load, render::StoreAction::Store, true);
        else {
            if (scenario == 1) Clear(graph, color, "discard", render::LoadAction::Clear, render::StoreAction::Discard);
            if (scenario == 2) Clear(graph, color, "only mip zero");
            graph.AddRasterPass<EmptyPass>("consumer", [=](EmptyPass&, RenderGraphRasterBuilder& builder) {
                if (scenario == 3) builder.SetColorAttachment(0, color, {.View = {.Range = {0, 1, 0, 1}}});
                else builder.SetColorAttachment(0, output);
                builder.ReadTexture(color, {.Range = {0, 1, scenario == 2 ? 1u : 0u, 1}});
                builder.SetSideEffect(); }, EmptyRaster);
        }
        EXPECT_FALSE(graph.Compile());
        EXPECT_FALSE(graph.GetReport().Diagnostics.empty());
        EXPECT_EQ(graph.GetReport().Diagnostics.front().Code, scenario == 3 ? "OverlappingAccess" : "UninitializedRead");
        EXPECT_EQ(Pool->GetStats().Created, 0u);
    }
}

TEST_F(RenderGraphCompileTest, RejectsCrossGraphHandlesUnsupportedDescriptorAndMismatchedAttachments) {
    auto first = MakeGraph();
    auto stale = first.CreateTexture(GraphColor(), "first");
    auto second = MakeGraph();
    Clear(second, stale, "wrong graph");
    EXPECT_FALSE(second.Compile());
    EXPECT_EQ(second.GetReport().Diagnostics[0].Code, "InvalidHandle");
    auto invalid = MakeGraph();
    auto desc = GraphColor();
    desc.SampleCount = 3;
    auto unsupported = invalid.CreateTexture(desc, "bad samples");
    Clear(invalid, unsupported);
    EXPECT_FALSE(invalid.Compile());
    auto mismatch = MakeGraph();
    auto a = mismatch.CreateTexture(GraphColor(), "a");
    desc = GraphColor();
    desc.Width = 8;
    auto b = mismatch.CreateTexture(desc, "b");
    mismatch.AddRasterPass<EmptyPass>("mismatched", [=](EmptyPass&, RenderGraphRasterBuilder& builder) {
        builder.SetColorAttachment(0, a); builder.SetColorAttachment(1, b); builder.SetSideEffect(); }, EmptyRaster);
    EXPECT_FALSE(mismatch.Compile());
    EXPECT_EQ(Pool->GetStats().Created, 0u);
}

TEST_F(RenderGraphCompileTest, ReadAfterWriteKeepsProducerAndIndependentRootsStayOrdered) {
    auto graph = MakeGraph();
    auto input = graph.CreateTexture(GraphColor(), "input");
    auto output = graph.CreateTexture(GraphColor(), "output");
    Clear(graph, input, "producer");
    graph.AddRasterPass<EmptyPass>("consumer", [=](EmptyPass&, RenderGraphRasterBuilder& builder) {
        builder.ReadTexture(input); builder.SetColorAttachment(0, output); builder.SetSideEffect(); }, EmptyRaster);
    Clear(graph, output, "overwrite root", render::LoadAction::Clear, render::StoreAction::Store, true);
    ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
    EXPECT_EQ(graph.GetReport().LivePasses, 3u);
    EXPECT_EQ(graph.GetReport().Passes[1].DataDependencies, vector<uint32_t>{0});
    EXPECT_EQ(graph.GetReport().Passes[2].HazardDependencies, vector<uint32_t>{1});
}

TEST_F(RenderGraphCompileTest, CapabilityRejectionHasCallerLocationAndDescriptor) {
    Device.RejectedFormat = render::TextureFormat::RGBA8_UNORM;
    auto graph = MakeGraph("capabilities");
    const auto location = std::source_location::current();
    auto color = graph.CreateTexture(GraphColor(), "unsupported", location);
    Clear(graph, color);
    EXPECT_FALSE(graph.Compile());
    ASSERT_FALSE(graph.GetReport().Diagnostics.empty());
    const auto& error = graph.GetReport().Diagnostics.front();
    EXPECT_EQ(error.Graph, "capabilities");
    EXPECT_EQ(error.Resource, "unsupported");
    EXPECT_EQ(error.File, location.file_name());
    EXPECT_EQ(error.Line, location.line());
    EXPECT_NE(error.Message.find("16x16x1"), string::npos);
    EXPECT_EQ(Pool->GetStats().Created, 0u);
}

TEST_F(RenderGraphCompileTest, DisjointMipsStayIndependentAndFullReadConsumesBoth) {
    auto graph = MakeGraph();
    const auto texture = graph.CreateTexture(GraphColor(2), "mips");
    const auto output = graph.CreateTexture(GraphColor(), "output");
    Clear(graph, texture, "mip zero");
    Clear(graph, texture, "mip one", render::LoadAction::Clear, render::StoreAction::Store, false, 1);
    graph.AddRasterPass<EmptyPass>("consume both", [=](EmptyPass&, RenderGraphRasterBuilder& builder) {
        builder.ReadTexture(texture); builder.SetColorAttachment(0, output); builder.SetSideEffect(); }, EmptyRaster);
    ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
    EXPECT_TRUE(graph.GetReport().Passes[1].DataDependencies.empty());
    EXPECT_TRUE(graph.GetReport().Passes[1].HazardDependencies.empty());
    EXPECT_EQ(graph.GetReport().Passes[2].DataDependencies, (vector<uint32_t>{0, 1}));
    EXPECT_EQ(graph.GetReport().Passes[2].HazardDependencies, (vector<uint32_t>{0, 1}));
}

TEST_F(RenderGraphCompileTest, DumpAndLargeGraphCompilationAreDeterministic) {
    string json, dot;
    for (uint32_t count : {100u, 1000u}) {
        const auto start = std::chrono::steady_clock::now();
        std::chrono::nanoseconds compileTime{};
        for (uint32_t iteration = 0; iteration < 100; ++iteration) {
            auto graph = MakeGraph("stable");
            for (uint32_t p = 0; p < count; ++p) graph.AddComputePass<EmptyPass>(fmt::format("pass {}", p), [](EmptyPass&, RenderGraphComputeBuilder& builder) { builder.SetSideEffect(); }, EmptyCompute);
            const auto compileStart = std::chrono::steady_clock::now();
            ASSERT_TRUE(graph.Compile());
            compileTime += std::chrono::steady_clock::now() - compileStart;
            EXPECT_EQ(graph.GetReport().LivePasses, count);
            if (iteration == 0) {
                json = graph.GetReport().ToJson();
                dot = graph.GetReport().ToDot();
            } else {
                EXPECT_EQ(graph.GetReport().ToJson(), json);
                EXPECT_EQ(graph.GetReport().ToDot(), dot);
            }
        }
        RecordProperty(fmt::format("compile_and_dump_{}_passes_us", count),
                       static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() / 100));
        RecordProperty(fmt::format("compile_{}_passes_us", count), static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(compileTime).count() / 100));
        RecordProperty(fmt::format("native_creates_{}_passes", count), Device.NativeCreates);
    }
}

}  // namespace
}  // namespace radray
