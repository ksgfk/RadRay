#include "graph_compile_device.h"
#include <radray/runtime/render_framework/render_graph.h>
#include <chrono>
#include <algorithm>
#include <random>
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

TEST_F(RenderGraphCompileTest, RejectsTextureReadbackOffsetInsideATexelBeforeAllocation) {
    auto graph = MakeGraph();
    auto color = graph.CreateTexture(GraphColor(), "color");
    Clear(graph, color);
    auto destination = graph.CreateBuffer({4096, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}}, "readback");
    graph.AddCopyTextureToBufferPass("unaligned texel", color, destination, {0, 1, 0, 1}, 31);
    EXPECT_FALSE(graph.Compile());
    EXPECT_NE(graph.GetReport().ToText().find("CopyTextureRange"), string::npos);
}

TEST_F(RenderGraphCompileTest, G06PartialWritesDoNotInventBufferRangeValidityAndInvalidCopiesAllocateNothing) {
    for (uint32_t scenario = 0; scenario < 4; ++scenario) {
        auto graph = MakeGraph("partial buffer validity");
        const auto source = graph.CreateBuffer({16, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::CopySource, {}}, "source");
        const auto target = graph.CreateBuffer({16, render::MemoryType::Device, render::BufferUse::CopySource | render::BufferUse::CopyDestination, {}}, "target");
        graph.AddComputePass<EmptyPass>("source initialized", [=](EmptyPass&, RenderGraphComputeBuilder& builder) { builder.WriteBuffer(source); }, EmptyCompute);
        const uint64_t size = scenario == 1 ? 0 : 8;
        const uint64_t offset = scenario == 2 ? 12 : scenario == 3 ? std::numeric_limits<uint64_t>::max() - 3
                                                                   : 0;
        graph.AddCopyBufferPass("partial copy", source, target, size, 0, offset);
        graph.AddComputePass<EmptyPass>("whole buffer consumer", [=](EmptyPass&, RenderGraphComputeBuilder& builder) { builder.ReadBuffer(target, RgBufferAccess::CopySource); builder.SetSideEffect(); }, EmptyCompute);
        EXPECT_FALSE(graph.Compile());
        ASSERT_FALSE(graph.GetReport().Diagnostics.empty());
        EXPECT_EQ(Device.NativeCreates, 0u);
    }
}
class CompileTexture final : public render::Texture {
public:
    explicit CompileTexture(render::TextureDescriptor descriptor) : Desc(descriptor) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    render::TextureDescriptor GetDesc() const noexcept override { return Desc; }
    render::TextureDescriptor Desc;
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

TEST_F(RenderGraphCompileTest, IndirectDeclarationsValidateAndCarryContentDependencies) {
    auto graph = MakeGraph("indirect");
    const auto arguments = graph.CreateBuffer(
        {sizeof(render::DrawIndexedIndirectArguments), render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::Indirect, {}},
        "arguments");
    const auto color = graph.CreateTexture(GraphColor(), "color");
    graph.AddComputePass<EmptyPass>(
        "produce", [=](EmptyPass&, RenderGraphComputeBuilder& builder) {
            builder.WriteBuffer(arguments);
        },
        EmptyCompute);
    graph.AddRasterPass<EmptyPass>(
        "consume", [=](EmptyPass&, RenderGraphRasterBuilder& builder) {
            builder.SetColorAttachment(0, color);
            EXPECT_TRUE(builder.ReadIndirectArguments(
                                   arguments, RgIndirectCommand::DrawIndexed)
                            .IsValid());
            builder.SetSideEffect();
        },
        EmptyRaster);
    ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
    EXPECT_EQ(graph.GetReport().Passes[1].DataDependencies, vector<uint32_t>{0});

    const auto expectInvalid = [&](render::BufferUses usage, RgIndirectCommand command,
                                   uint64_t offset, uint32_t count, bool indirectDraw,
                                   bool indirectDispatch) {
        Device.Capabilities.Features.IndirectDraw = indirectDraw;
        Device.Capabilities.Features.IndirectDispatch = indirectDispatch;
        auto invalid = MakeGraph("invalid indirect");
        const auto buffer = invalid.CreateBuffer(
            {32, render::MemoryType::Device, usage, {}}, "arguments");
        invalid.AddComputePass<EmptyPass>(
            "consume", [=](EmptyPass&, RenderGraphComputeBuilder& builder) {
                EXPECT_FALSE(builder.ReadIndirectArguments(buffer, command, offset, count).IsValid());
                builder.SetSideEffect();
            },
            EmptyCompute);
        EXPECT_FALSE(invalid.Compile());
        ASSERT_FALSE(invalid.GetReport().Diagnostics.empty());
    };
    expectInvalid(render::BufferUse::Resource, RgIndirectCommand::Dispatch, 0, 1, true, true);
    expectInvalid(render::BufferUse::Indirect, RgIndirectCommand::Dispatch, 2, 1, true, true);
    expectInvalid(render::BufferUse::Indirect, RgIndirectCommand::Dispatch, 24, 1, true, true);
    expectInvalid(render::BufferUse::Indirect, RgIndirectCommand::Dispatch, 0, 2, true, true);
    expectInvalid(render::BufferUse::Indirect, RgIndirectCommand::Dispatch, 0, 1, true, false);
    expectInvalid(render::BufferUse::Indirect, RgIndirectCommand::Draw, 0, 1, false, true);
    {
        auto owner = MakeGraph("indirect owner");
        const auto foreign = owner.CreateBuffer(
            {sizeof(render::DrawIndirectArguments), render::MemoryType::Device, render::BufferUse::Indirect, {}},
            "foreign arguments");
        auto consumer = MakeGraph("indirect consumer");
        consumer.AddRasterPass<EmptyPass>(
            "consume foreign", [=](EmptyPass&, RenderGraphRasterBuilder& builder) {
                EXPECT_FALSE(builder.ReadIndirectArguments(
                                        foreign, RgIndirectCommand::Draw)
                                 .IsValid());
                builder.SetSideEffect();
            },
            EmptyRaster);
        EXPECT_FALSE(consumer.Compile());
        ASSERT_FALSE(consumer.GetReport().Diagnostics.empty());
        EXPECT_EQ(consumer.GetReport().Diagnostics.front().Code, "InvalidHandle");
    }
    Device.Capabilities.Features.IndirectDraw = true;
    Device.Capabilities.Features.IndirectDispatch = true;
    EXPECT_EQ(Pool->GetStats().Created, 0u);
}

TEST_F(RenderGraphCompileTest, ResolveValidatesArrayRangesAndCullsUnusedWork) {
    const auto sourceDesc = [] {
        return render::TextureDescriptor{
            render::TextureDimension::Dim2DArray, 32, 16, 4, 1, 4, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}};
    };
    const auto destinationDesc = [] {
        return render::TextureDescriptor{
            render::TextureDimension::Dim2DArray, 32, 16, 4, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::CopyDestination | render::TextureUse::Resource, {}};
    };
    {
        auto graph = MakeGraph("resolve array");
        const auto source = graph.CreateTexture(sourceDesc(), "msaa");
        const auto destination = graph.CreateTexture(destinationDesc(), "resolved");
        graph.AddRasterPass<EmptyPass>(
            "msaa", [=](EmptyPass&, RenderGraphRasterBuilder& builder) {
                builder.SetColorAttachment(
                    0, source,
                    {.View = {.Dimension = render::TextureDimension::Dim2DArray,
                              .Range = {1, 2, 0, 1}}});
            },
            EmptyRaster);
        graph.AddResolveTexturePass(
            "resolve", source, destination, {1, 2, 0, 1}, {1, 2, 0, 1});
        graph.AddComputePass<EmptyPass>(
            "consume", [=](EmptyPass&, RenderGraphComputeBuilder& builder) {
                builder.ReadTexture(destination, {.Dimension = render::TextureDimension::Dim2DArray,
                                                  .Range = {1, 2, 0, 1}});
                builder.SetSideEffect();
            },
            EmptyCompute);
        ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
        ASSERT_EQ(graph.GetReport().Passes[1].Type, RgPassType::Resolve);
        EXPECT_EQ(graph.GetReport().Passes[1].DataDependencies, vector<uint32_t>{0});
        EXPECT_EQ(graph.GetReport().Passes[2].DataDependencies, vector<uint32_t>{1});
    }
    {
        auto graph = MakeGraph("culled resolve");
        const auto source = graph.CreateTexture(sourceDesc(), "msaa");
        const auto destination = graph.CreateTexture(destinationDesc(), "resolved");
        graph.AddRasterPass<EmptyPass>(
            "msaa", [=](EmptyPass&, RenderGraphRasterBuilder& builder) {
                builder.SetColorAttachment(
                    0, source,
                    {.View = {.Dimension = render::TextureDimension::Dim2DArray,
                              .Range = {0, 1, 0, 1}}});
            },
            EmptyRaster);
        graph.AddResolveTexturePass(
            "unused resolve", source, destination, {0, 1, 0, 1}, {0, 1, 0, 1});
        ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
        EXPECT_EQ(graph.GetReport().LivePasses, 0u);
    }

    const auto expectInvalid = [&](render::TextureDescriptor source,
                                   render::TextureDescriptor destination) {
        auto graph = MakeGraph("invalid resolve");
        const auto src = graph.CreateTexture(source, "source");
        const auto dst = graph.CreateTexture(destination, "destination");
        graph.AddResolveTexturePass("resolve", src, dst);
        EXPECT_FALSE(graph.Compile());
        ASSERT_FALSE(graph.GetReport().Diagnostics.empty());
        EXPECT_EQ(graph.GetReport().Diagnostics.front().Code, "ResolveTextureDescriptor");
    };
    auto source = sourceDesc();
    auto destination = destinationDesc();
    source.Format = render::TextureFormat::BGRA8_UNORM;
    expectInvalid(source, destination);
    source = sourceDesc();
    destination.Width = 31;
    expectInvalid(source, destination);
    destination = destinationDesc();
    source.SampleCount = 1;
    expectInvalid(source, destination);
    source = sourceDesc();
    destination.SampleCount = 4;
    expectInvalid(source, destination);
    source = sourceDesc();
    destination = destinationDesc();
    source.Format = render::TextureFormat::D32_FLOAT;
    destination.Format = render::TextureFormat::D32_FLOAT;
    source.Usage = render::TextureUse::DepthStencilWrite | render::TextureUse::CopySource;
    destination.Usage = render::TextureUse::DepthStencilRead | render::TextureUse::CopyDestination;
    expectInvalid(source, destination);
    EXPECT_EQ(Pool->GetStats().Created, 0u);
}

TEST_F(RenderGraphCompileTest, RasterUavStagesAreCheckedBeforeAllocation) {
    const auto populate = [&](RenderGraph& graph) {
        auto color = graph.CreateTexture(GraphColor(), "attachment");
        auto storageDesc = GraphColor();
        storageDesc.Usage |= render::TextureUse::UnorderedAccess;
        auto storage = graph.CreateTexture(storageDesc, "storage");
        graph.AddRasterPass<EmptyPass>(
            "write", [=](EmptyPass&, RenderGraphRasterBuilder& builder) {
                builder.SetColorAttachment(0, color);
                builder.WriteTexture(storage, render::ShaderStage::Pixel);
                builder.SetSideEffect();
            },
            EmptyRaster);
    };
    Device.Capabilities.Features.UavWriteStages = render::ShaderStage::Compute;
    auto rejected = MakeGraph("raster uav rejected");
    populate(rejected);
    EXPECT_FALSE(rejected.Compile());
    ASSERT_FALSE(rejected.GetReport().Diagnostics.empty());
    EXPECT_EQ(rejected.GetReport().Diagnostics.back().Code, "UnsupportedUavStage");

    Device.Capabilities.Features.UavWriteStages =
        render::ShaderStage::Graphics | render::ShaderStage::Compute;
    auto supported = MakeGraph("raster uav supported");
    populate(supported);
    EXPECT_TRUE(supported.Compile()) << supported.GetReport().ToText();
    EXPECT_EQ(Pool->GetStats().Created, 0u);
}

TEST_F(RenderGraphCompileTest, G05UnusedSixPassEffectsAreCulledFromObservableConsumers) {
    for (uint32_t consumed = 0; consumed <= 2; ++consumed) {
        auto graph = MakeGraph("effect consumers");
        CompileTexture output{GraphColor()};
        array<render::TextureStates, 1> states{render::TextureState::Undefined};
        array<uint8_t, 1> valid{0};
        RenderExternalTexture external{&output, output.Desc, states, valid};
        const auto target = graph.ImportTexture(external, "observable output", RenderGraphExternalAccess::ObservableOutput);
        array<RgTextureHandle, 3> ends;
        for (uint32_t chain = 0; chain < 3; ++chain) {
            RgTextureHandle previous;
            for (uint32_t step = 0; step < 6; ++step) {
                const auto resource = graph.CreateTexture(GraphColor(), fmt::format("effect {} step {}", chain, step));
                graph.AddRasterPass<EmptyPass>(fmt::format("chain {} pass {}", chain, step), [=](EmptyPass&, RenderGraphRasterBuilder& builder) {
                    if (step) builder.ReadTexture(previous);
                    builder.SetColorAttachment(0, resource); }, EmptyRaster);
                previous = resource;
            }
            ends[chain] = previous;
        }
        graph.AddRasterPass<EmptyPass>("composite", [=](EmptyPass&, RenderGraphRasterBuilder& builder) {
            for (uint32_t chain = 0; chain < consumed; ++chain) builder.ReadTexture(ends[chain]);
            builder.SetColorAttachment(0, target); }, EmptyRaster);
        ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
        const auto& report = graph.GetReport();
        EXPECT_EQ(report.LivePasses, consumed * 6 + 1);
        for (uint32_t p = 0; p < 18; ++p) {
            EXPECT_EQ(report.Passes[p].Live, p / 6 < consumed);
            EXPECT_EQ(report.Resources[p + 1].FirstUse >= 0, p / 6 < consumed);
            EXPECT_FALSE(report.Passes[p].Executed);
        }
        EXPECT_EQ(report.PhysicalAllocations, 0u);
    }
}

TEST_F(RenderGraphCompileTest, G08DependentGraphsMatchReferenceAndDeterministicPerformanceSamples) {
    for (uint32_t count : {100u, 1000u})
        for (uint32_t shape = 0; shape < 3; ++shape) {
            std::mt19937 random{20260906};
            vector<vector<uint32_t>> reference(count);
            for (uint32_t p = 1; p < count; ++p) {
                if (shape == 0)
                    reference[p].push_back(p - 1);
                else {
                    const uint32_t edges = 1 + random() % std::min(4u, p);
                    while (reference[p].size() < edges) {
                        const auto producer = random() % p;
                        if (std::find(reference[p].begin(), reference[p].end(), producer) == reference[p].end()) reference[p].push_back(producer);
                    }
                    std::sort(reference[p].begin(), reference[p].end());
                }
            }
            vector<bool> live(count, false);
            vector<uint32_t> pending{count - 1};
            while (!pending.empty()) {
                const auto p = pending.back();
                pending.pop_back();
                if (live[p]) continue;
                live[p] = true;
                pending.insert(pending.end(), reference[p].begin(), reference[p].end());
            }
            vector<double> micros;
            string json, dot;
            uint64_t reportBytes = 0, reportBlocks = 0;
            for (uint32_t iteration = 0; iteration < 100; ++iteration) {
                auto graph = MakeGraph("dependent benchmark");
                auto descriptor = GraphColor();
                descriptor.Usage |= render::TextureUse::UnorderedAccess;
                CompileTexture output{descriptor};
                array<render::TextureStates, 1> states{render::TextureState::Undefined};
                array<uint8_t, 1> valid{0};
                RenderExternalTexture external{&output, output.Desc, states, valid};
                const auto target = graph.ImportTexture(external, "observable final content", RenderGraphExternalAccess::ObservableOutput);
                vector<RgTextureHandle> textures;
                if (shape == 2) {
                    descriptor.Dim = render::TextureDimension::Dim2DArray;
                    descriptor.DepthOrArraySize = 3;
                    descriptor.MipLevels = 4;
                }
                const uint32_t cells = shape == 2 ? 12 : 1;
                for (uint32_t r = 0; r < (count + cells - 1) / cells; ++r) textures.push_back(graph.CreateTexture(descriptor, fmt::format("texture {}", r)));
                const auto view = [=](uint32_t p) { return RgTextureViewDesc{.Dimension = descriptor.Dim, .Range = {(p % cells) / 4, 1, (p % cells) % 4, 1}}; };
                for (uint32_t p = 0; p < count; ++p) graph.AddComputePass<EmptyPass>(fmt::format("pass {}", p), [&](EmptyPass&, RenderGraphComputeBuilder& builder) {
                for (const auto producer : reference[p]) builder.ReadTexture(textures[producer / cells], view(producer));
                builder.WriteTexture(textures[p / cells], view(p));
                if (p == count - 1) builder.WriteTexture(target); }, EmptyCompute);
                const auto compileStart = std::chrono::steady_clock::now();
                ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
                micros.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - compileStart).count());
                const auto& report = graph.GetReport();
                EXPECT_EQ(report.LivePasses, std::count(live.begin(), live.end(), true));
                for (uint32_t p = 0; p < count; ++p) {
                    ASSERT_EQ(report.Passes[p].Live, live[p]);
                    auto edges = report.Passes[p].DataDependencies;
                    std::sort(edges.begin(), edges.end());
                    ASSERT_EQ(edges, reference[p]);
                    for (const auto dep : edges) EXPECT_LT(dep, p);  // Declaration order is the independent stable topological order.
                }
                if (iteration == 0) {
                    json = report.ToJson();
                    dot = report.ToDot();
                    reportBytes = report.Passes.capacity() * sizeof(RenderGraphPassReport) + report.Resources.capacity() * sizeof(RenderGraphResourceReport);
                    reportBlocks = 2;
                    for (const auto& pass : report.Passes) {
                        reportBytes += (pass.DataDependencies.capacity() + pass.HazardDependencies.capacity()) * sizeof(uint32_t);
                        reportBlocks += !pass.DataDependencies.empty();
                        reportBlocks += !pass.HazardDependencies.empty();
                    }
                } else {
                    ASSERT_EQ(report.ToJson(), json);
                    ASSERT_EQ(report.ToDot(), dot);
                }
            }
            std::sort(micros.begin(), micros.end());
            const auto prefix = fmt::format("{}_{}_", count, shape == 0 ? "chain" : shape == 1 ? "fanout"
                                                                                               : "mips");
            RecordProperty(prefix + "compile_median_us", fmt::format("{:.3f}", micros[50]));
            RecordProperty(prefix + "compile_p95_us", fmt::format("{:.3f}", micros[94]));
            RecordProperty(prefix + "report_capacity_bytes", std::to_string(reportBytes));
            RecordProperty(prefix + "report_vector_allocations", std::to_string(reportBlocks));
            RecordProperty(prefix + "native_creates", Device.NativeCreates);
            RecordProperty(prefix + "repeats", 100);
        }
}

}  // namespace
}  // namespace radray
