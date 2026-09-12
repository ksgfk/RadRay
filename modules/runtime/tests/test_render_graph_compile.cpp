#include "graph_compile_device.h"
#include "failing_graph_command.h"
#include "render_graph_test_driver.h"
#include "upload_test_support.h"
#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/render_graph_runtime.h>
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
RgPassHandle Clear(RenderGraph& graph, RgTextureValue& texture, std::string_view name = "clear", render::LoadAction load = render::LoadAction::Clear,
                   render::StoreAction store = render::StoreAction::Store, bool root = false, uint32_t mip = 0) {
    texture = graph.NextVersion(texture);
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

TEST_F(RenderGraphCompileTest, RejectsUnsupportedDepthCopiesBeforeAllocation) {
    for (uint32_t operation = 0; operation < 3; ++operation) {
        auto graph = MakeGraph();
        const render::TextureDescriptor desc{render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::D24_UNORM_S8_UINT, render::MemoryType::Device, render::TextureUse::DepthStencilWrite | render::TextureUse::CopySource | render::TextureUse::CopyDestination, {}};
        const auto depth = graph.CreateTexture(desc, "depth");
        graph.AddRasterPass<EmptyPass>("initialize", [=](EmptyPass&, RenderGraphRasterBuilder& builder) { builder.SetDepthAttachment(depth); }, EmptyRaster);
        if (operation == 0) {
            const auto destination = graph.CreateTexture(desc, "destination");
            graph.AddCopyTexturePass("depth copy", depth, destination);
        } else if (operation == 1) {
            const auto destination = graph.CreateBuffer({4096, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}}, "readback");
            graph.AddCopyTextureToBufferPass("depth readback", depth, destination);
        } else {
            EXPECT_FALSE(graph.ReadbackTexture("owned depth readback", depth).IsValid());
        }
        EXPECT_FALSE(graph.Compile());
        ASSERT_FALSE(graph.GetReport().Diagnostics.empty());
        const char* expected[]{"CopyTextureDescriptor", "CopyTextureRange", "ReadbackFormat"};
        EXPECT_EQ(graph.GetReport().Diagnostics.front().Code, expected[operation]);
        EXPECT_EQ(Device.NativeCreates, 0u);
    }
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

TEST_F(RenderGraphCompileTest, ExecutionPlanVariantsBorrowCompleteResultsAcrossFlightsAndSurviveEviction) {
    auto plans = make_shared<RenderGraphPlanCache>(2);
    array<unique_ptr<RenderGraphFrameResources>, 3> flights;
    array<HostWriteBatch, 3> writes;
    for (auto& flight : flights) flight = make_unique<RenderGraphFrameResources>(Device, *Registry, plans);
    array<const CompiledRenderGraph*, 2> results{};
    array<uint64_t, 2> identities{};
    vector<unique_ptr<RenderGraph>> retained;
    for (uint32_t frame = 0; frame < 1000; ++frame) {
        const uint32_t flight = frame % 3, variant = frame % 2;
        flights[flight]->BeginFlight(frame + 1, writes[flight]);
        auto graph = make_unique<RenderGraph>(Device, *flights[flight], *Registry, "complete variants");
        auto color = graph->CreateTexture(GraphColor(), "color");
        Clear(*graph, color, "first");
        Clear(*graph, color, "last", variant ? render::LoadAction::Clear : render::LoadAction::Load, render::StoreAction::Store, true);
        ASSERT_TRUE(graph->Compile()) << graph->GetReport().ToText();
        const auto& report = graph->GetReport();
        EXPECT_EQ(report.LivePasses, variant ? 1u : 2u);
        EXPECT_EQ(report.MergedRasterPasses, variant ? 0u : 1u);
        if (frame < 2) {
            EXPECT_FALSE(report.CompilePlanReused);
            EXPECT_EQ(report.NormalizeBuilds, 1u);
            EXPECT_EQ(report.StoragePlanBuilds, 1u);
            results[variant] = &graph->GetCompiledGraph();
            identities[variant] = report.ExecutionPlanId;
            retained.push_back(std::move(graph));
        } else {
            EXPECT_TRUE(report.CompilePlanReused);
            EXPECT_EQ(report.ExecutionPlanId, identities[variant]);
            EXPECT_EQ(&graph->GetCompiledGraph(), results[variant]);
            EXPECT_EQ(report.NormalizeBuilds + report.IrBuilds + report.TopologyBuilds + report.StoragePlanBuilds + report.RasterPlanBuilds + report.ExecutionPlanBuilds + report.BarrierTemplateBuilds + report.RoutePlanBuilds, 0u);
        }
    }
    EXPECT_EQ(plans->Size(), 2u);
    plans->Clear();
    EXPECT_EQ(plans->Size(), 0u);
    EXPECT_EQ(retained[0]->GetReport().ExecutionPlanId, identities[0]);
    EXPECT_EQ(retained[0]->GetCompiledGraph().ExecutionOrder.size(), 2u);
    EXPECT_EQ(retained[1]->GetCompiledGraph().ExecutionOrder.size(), 1u);
}

TEST_F(RenderGraphCompileTest, TemplateVariantsBorrowDeclarationsAndAppendDynamicVersionTailsAcrossFlights) {
    struct Recipe {
        unique_ptr<string> Owned;
    };
    struct Frame {
        uint32_t Value{0};
    };
    array<shared_ptr<const RenderGraphTemplate>, 2> templates;
    array<RgTemplateSlot<Frame>, 2> slots;
    array<RgBufferValue, 2> outputs;
    uint32_t declarationCalls = 0;
    for (uint32_t variant = 0; variant < 2; ++variant) {
        auto builder = MakeGraph("template builder");
        slots[variant] = builder.DeclareTemplateSlot<Frame>();
        auto value = builder.CreateBuffer({64, render::MemoryType::Device, render::BufferUse::UnorderedAccess, {}}, "owned buffer");
        for (uint32_t stage = 0; stage < 3 + variant; ++stage) {
            if (stage) value = builder.NextVersion(value);
            builder.AddTemplateComputePass<Recipe>("stable stage", slots[variant], [&](Recipe& recipe, RenderGraphComputeBuilder& pass) {
                ++declarationCalls;
                recipe.Owned = make_unique<string>("long immutable recipe that cannot be copied by its C++ type");
                if (stage) pass.ReadWriteBuffer(value); else pass.WriteBuffer(value); }, +[](const Recipe& recipe, Frame&, RenderGraphPrepareContext&) { return !recipe.Owned->empty(); }, +[](const Recipe&, const Frame&, RenderGraphComputeContext&) {});
        }
        outputs[variant] = value;
        templates[variant] = builder.FreezeTemplate();
        ASSERT_TRUE(templates[variant]) << builder.GetReport().ToText();
    }
    EXPECT_EQ(declarationCalls, 7u);
    auto plans = make_shared<RenderGraphPlanCache>(2);
    array<unique_ptr<RenderGraphFrameResources>, 3> flights;
    array<HostWriteBatch, 3> writes;
    for (auto& flight : flights) flight = make_unique<RenderGraphFrameResources>(Device, *Registry, plans);
    array<const CompiledRenderGraph*, 2> identities{};
    vector<unique_ptr<RenderGraph>> retained;
    for (uint32_t frame = 0; frame < 1000; ++frame) {
        const auto flight = frame % 3, variant = frame % 2;
        flights[flight]->BeginFlight(frame + 1, writes[flight]);
        auto graph = make_unique<RenderGraph>(Device, *flights[flight], *Registry, "template and dynamic fragment");
        const auto instance = graph->Instantiate(templates[variant]);
        ASSERT_TRUE(instance.IsValid());
        ASSERT_TRUE(instance.Bind(slots[variant], make_shared<Frame>(Frame{frame})));
        const auto value = instance.Value(outputs[variant]);
        EXPECT_EQ(value.Generation, graph->GetGeneration());
        EXPECT_FALSE(instance.Value(outputs[1 - variant]).IsValid());
        const auto next = graph->NextVersion(value);
        ASSERT_TRUE(next.IsValid());
        graph->AddComputePass<EmptyPass>("dynamic fragment", [=](EmptyPass&, RenderGraphComputeBuilder& pass) { pass.ReadWriteBuffer(next); pass.SetSideEffect(); }, EmptyCompute);
        ASSERT_TRUE(graph->Compile()) << graph->GetReport().ToText();
        const auto& report = graph->GetReport();
        EXPECT_EQ(report.ResourceDeclarations, 0u);
        EXPECT_EQ(report.PassDeclarations, 1u);
        EXPECT_EQ(report.TemplateInstances, 1u);
        EXPECT_EQ(report.LivePasses, 4 + variant);
        if (frame < 2) {
            EXPECT_EQ(report.TemplatePlacementBuilds, 1u);
            EXPECT_EQ(report.TemplateMaterializations, 1u);
            identities[variant] = &graph->GetCompiledGraph();
            retained.push_back(std::move(graph));
        } else {
            EXPECT_TRUE(report.CompilePlanReused);
            EXPECT_EQ(&graph->GetCompiledGraph(), identities[variant]);
            EXPECT_EQ(report.PortResolveBuilds + report.TemplatePlacementBuilds + report.TemplateMaterializations + report.NormalizeBuilds + report.IrBuilds + report.TopologyBuilds + report.StoragePlanBuilds + report.RasterPlanBuilds + report.ExecutionPlanBuilds, 0u);
        }
    }
    EXPECT_EQ(declarationCalls, 7u);
    plans->Clear();
    templates = {};
    EXPECT_EQ(retained[0]->GetCompiledGraph().ExecutionOrder.size(), 4u);
    EXPECT_EQ(retained[1]->GetCompiledGraph().ExecutionOrder.size(), 5u);
}

TEST_F(RenderGraphCompileTest, TemplateSlotsRejectWrongTypesAndLegacyCaptures) {
    auto builder = MakeGraph();
    const auto slot = builder.DeclareTemplateSlot<int>();
    builder.AddTemplateComputePass<EmptyPass>("typed", slot, [](EmptyPass&, RenderGraphComputeBuilder& pass) { pass.SetSideEffect(); }, +[](const EmptyPass&, int&, RenderGraphPrepareContext&) { return true; }, +[](const EmptyPass&, const int&, RenderGraphComputeContext&) {});
    const auto graphTemplate = builder.FreezeTemplate();
    ASSERT_TRUE(graphTemplate);
    for (uint32_t scenario = 0; scenario < 3; ++scenario) {
        auto graph = MakeGraph();
        const auto instance = graph.Instantiate(graphTemplate);
        if (scenario == 0)
            EXPECT_FALSE(instance.Bind(RgTemplateSlot<float>{slot.Index, slot.Generation}, make_shared<float>(1.f)));
        else if (scenario == 1)
            EXPECT_FALSE(instance.Bind(RgTemplateSlot<int>{slot.Index, graph.GetGeneration()}, make_shared<int>(1)));
        else {
            ASSERT_TRUE(instance.Bind(slot, make_shared<int>(1)));
            EXPECT_FALSE(instance.Bind(slot, make_shared<int>(2)));
        }
        EXPECT_EQ(graph.GetFirstErrorCode(), "TemplateSlot");
        EXPECT_FALSE(graph.Compile());
    }
    auto legacy = MakeGraph();
    legacy.AddComputePass<EmptyPass>("capture", [](EmptyPass&, RenderGraphComputeBuilder& pass) { pass.SetSideEffect(); }, EmptyCompute);
    EXPECT_FALSE(legacy.FreezeTemplate());
    EXPECT_EQ(legacy.GetFirstErrorCode(), "TemplateCapture");
}

TEST_F(RenderGraphCompileTest, TemplatePortsPatchExternalIdentityAndPreserveInitialContentValidation) {
    struct Recipe {
        RgTextureViewHandle Source;
    };
    auto builder = MakeGraph("port template");
    const auto slot = builder.DeclareTemplateSlot<int>();
    const auto input = builder.DeclareTexturePort(GraphColor(), "input");
    const auto output = builder.CreateTexture(GraphColor(), "output");
    builder.AddTemplateRasterPass<Recipe>("sample", slot, [&](Recipe& recipe, RenderGraphRasterBuilder& pass) {
        recipe.Source = pass.ReadTexture(builder.Value(input));
        pass.SetColorAttachment(0, output); }, +[](const Recipe& recipe, int&, RenderGraphPrepareContext& context) { return context.GetTextureView(recipe.Source) != nullptr; }, +[](const Recipe&, const int&, RenderGraphRasterContext&) {});
    const auto graphTemplate = builder.FreezeTemplate();
    ASSERT_TRUE(graphTemplate);
    RenderGraphFrameResources resources{Device, *Registry};
    HostWriteBatch writes;
    CompileTexture a{GraphColor()}, b{GraphColor()};
    array<render::TextureStates, 1> states{render::TextureState::ShaderRead};
    array<uint8_t, 1> valid{1};
    RenderExternalTexture external{&a, a.Desc, states, valid};
    uint64_t identity = 0;
    for (uint32_t frame = 0; frame < 5; ++frame) {
        resources.BeginFlight(frame + 1, writes);
        external.Texture = frame % 2 ? &b : &a;
        states[0] = frame % 2 ? render::TextureState::RenderTarget : render::TextureState::ShaderRead;
        valid[0] = frame == 2 ? 0 : 1;
        RenderGraph graph{Device, resources, *Registry, "port frame"};
        const auto imported = graph.ImportTexture(external, "current native", RenderGraphExternalAccess::ReadOnly);
        for (uint32_t instanceIndex = 0; instanceIndex < 2; ++instanceIndex) {
            const auto instance = graph.Instantiate(graphTemplate);
            ASSERT_TRUE(instance.Bind(slot, make_shared<int>(frame)));
            ASSERT_TRUE(graph.Connect(instance.Value(input), imported));
            graph.ExportTexture(instance.Value(output), render::TextureState::ShaderRead);
        }
        if (frame == 4) {
            RenderGraphCompileOptions options;
            options.ReuseCompiledPlan = false;
            graph.SetCompileOptions(options);
        }
        if (frame == 2) {
            EXPECT_FALSE(graph.Compile());
            EXPECT_FALSE(graph.GetReport().CompilePlanReused);
            continue;
        }
        ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
        const auto& report = graph.GetReport();
        EXPECT_EQ(report.LivePasses, 4u);
        for (uint32_t pass : {0u, 2u}) {
            ASSERT_EQ(report.Passes[pass].Accesses.size(), 2u);
            EXPECT_EQ(report.Passes[pass].Accesses.front().Resource, imported.Index);
        }
        if (frame == 0)
            identity = report.ExecutionPlanId;
        else if (frame == 4) {
            EXPECT_NE(report.ExecutionPlanId, identity);
            EXPECT_EQ(report.TemplateMaterializations, 1u);
        } else {
            EXPECT_EQ(report.ExecutionPlanId, identity);
            EXPECT_EQ(report.TemplatePlacementBuilds + report.TemplateMaterializations + report.PortResolveBuilds, 0u);
        }
    }
}

TEST_F(RenderGraphCompileTest, ExternalTemplateSlotsReusePlansAcrossNativeChangesAndTrackAliases) {
    struct Recipe {};
    auto builder = MakeGraph("external slots");
    const auto frameSlot = builder.DeclareTemplateSlot<int>();
    const auto firstSlot = builder.DeclareExternalTexture(GraphColor(), "first", RenderGraphExternalAccess::ReadOnly);
    const auto secondSlot = builder.DeclareExternalTexture(GraphColor(), "second", RenderGraphExternalAccess::ReadOnly);
    const auto output = builder.CreateTexture(GraphColor(), "output");
    builder.AddTemplateRasterPass<Recipe>("read slots", frameSlot, [&](Recipe&, RenderGraphRasterBuilder& pass) {
        pass.ReadTexture(firstSlot);
        pass.ReadTexture(secondSlot);
        pass.SetColorAttachment(0, output); }, +[](const Recipe&, int&, RenderGraphPrepareContext&) { return true; }, +[](const Recipe&, const int&, RenderGraphRasterContext&) {});
    builder.ExportTexture(output, render::TextureState::ShaderRead);
    const auto graphTemplate = builder.FreezeTemplate();
    ASSERT_TRUE(graphTemplate);
    RenderGraphFrameResources resources{Device, *Registry};
    HostWriteBatch writes;
    CompileTexture a{GraphColor()}, b{GraphColor()};
    array<render::TextureStates, 1> states{render::TextureState::ShaderRead};
    array<uint8_t, 1> valid{1};
    RenderExternalTexture first{&a, a.Desc, states, valid};
    RenderExternalTexture second{&b, b.Desc, states, valid};
    array<uint64_t, 2> identities{};
    for (uint32_t frame = 0; frame < 6; ++frame) {
        resources.BeginFlight(frame + 1, writes);
        const bool alias = frame % 2 == 0;
        first.Texture = frame < 2 ? &a : &b;
        second.Texture = alias ? first.Texture : frame < 2 ? &b
                                                           : &a;
        states[0] = frame < 2 ? render::TextureState::ShaderRead : render::TextureState::RenderTarget;
        valid[0] = frame == 4 ? 0 : 1;
        RenderGraph graph{Device, resources, *Registry, "slot frame"};
        const auto instance = graph.Instantiate(graphTemplate);
        ASSERT_TRUE(instance.Bind(frameSlot, make_shared<int>(frame)));
        ASSERT_TRUE(instance.Bind(firstSlot, first));
        ASSERT_TRUE(instance.Bind(secondSlot, second));
        if (frame == 4) {
            EXPECT_FALSE(graph.Compile());
            EXPECT_FALSE(graph.GetReport().CompilePlanReused);
            continue;
        }
        ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
        const auto& report = graph.GetReport();
        EXPECT_EQ(report.ResourceDeclarations + report.PassDeclarations, 0u);
        ASSERT_EQ(report.Passes[0].Accesses.size(), 3u);
        EXPECT_EQ(report.Passes[0].Accesses[0].Resource, instance.Value(firstSlot).Index);
        EXPECT_EQ(report.Passes[0].Accesses[1].Resource, instance.Value(alias ? firstSlot : secondSlot).Index);
        if (frame < 2)
            identities[frame % 2] = report.ExecutionPlanId;
        else {
            EXPECT_EQ(report.ExecutionPlanId, identities[frame % 2]);
            EXPECT_EQ(report.TemplateMaterializations + report.PortResolveBuilds + report.IrBuilds, 0u);
        }
    }
    {
        auto graph = MakeGraph();
        const auto instance = graph.Instantiate(graphTemplate);
        ASSERT_TRUE(instance.Bind(frameSlot, make_shared<int>(0)));
        ASSERT_TRUE(instance.Bind(firstSlot, first));
        EXPECT_FALSE(graph.Compile());
        EXPECT_EQ(graph.GetFirstErrorCode(), "TemplateExternal");
    }
    {
        auto graph = MakeGraph();
        const auto instance = graph.Instantiate(graphTemplate);
        ASSERT_TRUE(instance.Bind(firstSlot, first));
        EXPECT_FALSE(instance.Bind(firstSlot, first));
        EXPECT_EQ(graph.GetFirstErrorCode(), "TemplateExternal");
    }
}

TEST_F(RenderGraphCompileTest, AliasedExternalTemplateWritesCannotBranchPhysicalStorage) {
    struct Recipe {};
    auto builder = MakeGraph("external writer aliases");
    const auto frameSlot = builder.DeclareTemplateSlot<int>();
    array<RgTextureValue, 2> slots;
    for (auto& slot : slots) {
        slot = builder.DeclareExternalTexture(GraphColor(), "writer", RenderGraphExternalAccess::ReadWrite);
        const auto output = builder.NextVersion(slot);
        builder.AddTemplateRasterPass<Recipe>("write", frameSlot, [=](Recipe&, RenderGraphRasterBuilder& pass) {
            pass.SetColorAttachment(0, output);
            pass.SetSideEffect(); }, +[](const Recipe&, int&, RenderGraphPrepareContext&) { return true; }, +[](const Recipe&, const int&, RenderGraphRasterContext&) {});
    }
    const auto graphTemplate = builder.FreezeTemplate();
    ASSERT_TRUE(graphTemplate);
    CompileTexture native{GraphColor()};
    array<render::TextureStates, 1> states{render::TextureState::ShaderRead};
    array<uint8_t, 1> valid{1};
    RenderExternalTexture external{&native, native.Desc, states, valid};
    auto graph = MakeGraph();
    const auto instance = graph.Instantiate(graphTemplate);
    ASSERT_TRUE(instance.Bind(frameSlot, make_shared<int>(0)));
    for (auto slot : slots) ASSERT_TRUE(instance.Bind(slot, external));
    EXPECT_FALSE(graph.Compile());
}

TEST_F(RenderGraphCompileTest, ExecutionPlanSeparatesExternalAddressAndStateFromResourceShape) {
    RenderGraphFrameResources resources{Device, *Registry};
    HostWriteBatch writes;
    CompileTexture first{GraphColor()}, second{GraphColor()};
    array<render::TextureStates, 1> states{render::TextureState::RenderTarget};
    array<uint8_t, 1> valid{1};
    RenderExternalTexture external{&first, first.Desc, states, valid};
    uint64_t planId = 0;
    for (uint32_t frame = 0; frame < 4; ++frame) {
        resources.BeginFlight(frame + 1, writes);
        external.Texture = frame % 2 ? &second : &first;
        states[0] = frame % 2 ? render::TextureState::ShaderRead : render::TextureState::RenderTarget;
        if (frame == 3) {
            second.Desc.Width = external.Desc.Width = 32;
        }
        RenderGraph graph{Device, resources, *Registry, "external patch"};
        const auto input = graph.ImportTexture(external, "logical input", RenderGraphExternalAccess::ReadOnly);
        const auto output = graph.CreateTexture(GraphColor(), "output");
        graph.AddRasterPass<EmptyPass>("consume", [&](EmptyPass&, RenderGraphRasterBuilder& builder) {
            builder.ReadTexture(input);
            builder.SetColorAttachment(0, output);
            builder.SetSideEffect(); }, EmptyRaster);
        ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
        if (frame == 0)
            planId = graph.GetReport().ExecutionPlanId;
        else if (frame < 3) {
            EXPECT_EQ(graph.GetReport().ExecutionPlanId, planId);
            EXPECT_TRUE(graph.GetReport().CompilePlanReused);
        } else {
            EXPECT_NE(graph.GetReport().ExecutionPlanId, planId);
            EXPECT_FALSE(graph.GetReport().CompilePlanReused);
        }
    }
}

vector<uint64_t> BarrierWords(std::span<const render::ResourceBarrierDescriptor> barriers) {
    vector<uint64_t> result;
    for (const auto& barrier : barriers) {
        result.push_back(barrier.index());
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            result.push_back(reinterpret_cast<uintptr_t>(value.Target));
            if constexpr (!std::is_same_v<T, render::BarrierUavDescriptor>) {
                result.push_back(value.Before.value());
                result.push_back(value.After.value());
                result.push_back(value.BeforeStages.value());
                result.push_back(value.AfterStages.value());
                result.push_back(reinterpret_cast<uintptr_t>(value.OtherQueue.Get()));
                result.push_back(value.IsFromOrToOtherQueue);
            }
            if constexpr (std::is_same_v<T, render::BarrierTextureDescriptor>) {
                result.push_back(value.IsSubresourceBarrier);
                result.push_back(value.Range.BaseArrayLayer);
                result.push_back(value.Range.ArrayLayerCount);
                result.push_back(value.Range.BaseMipLevel);
                result.push_back(value.Range.MipLevelCount);
                result.push_back(value.Range.Aspects.value());
            }
            if constexpr (std::is_same_v<T, render::BarrierBufferDescriptor>) {
                result.push_back(value.Range.Offset);
                result.push_back(value.Range.Size);
            }
        },
                   barrier);
    }
    return result;
}

TEST(RenderGraphPlanExecutionTest, BarrierTemplatesPatchAddressesStatesAndCommitOnlyOnSubmission) {
    for (const bool eliminate : {false, true}) {
        test::UploadTestDevice device;
        render::RenderPassRegistry registry{&device};
        RenderGraphFrameResources cached{device, registry}, forced{device, registry};
        HostWriteBatch writes;
        const render::BufferDescriptor bufferDesc{64, render::MemoryType::Device, render::BufferUse::CopySource | render::BufferUse::CopyDestination | render::BufferUse::Resource | render::BufferUse::Vertex, {}};
        test::UploadTestBuffer sourceA{&device, bufferDesc, device.LiveDeviceBuffers}, sourceB{&device, bufferDesc, device.LiveDeviceBuffers};
        test::UploadTestBuffer targetA{&device, bufferDesc, device.LiveDeviceBuffers}, targetB{&device, bufferDesc, device.LiveDeviceBuffers};
        auto textureDesc = GraphColor(2);
        textureDesc.Dim = render::TextureDimension::Dim2DArray;
        textureDesc.DepthOrArraySize = 2;
        textureDesc.Usage |= render::TextureUse::UnorderedAccess;
        test::UploadTestTexture textureA{textureDesc, device.LiveTextures}, textureB{textureDesc, device.LiveTextures};
        uint64_t identity = 0;
        for (uint32_t frame = 0; frame < 6; ++frame) {
            vector<uint64_t> reference;
            for (const bool reuse : {false, true}) {
                auto& resources = reuse ? cached : forced;
                resources.BeginFlight(frame + 1, writes);
                auto* source = frame % 2 ? &sourceB : &sourceA;
                auto* target = frame % 2 ? &targetB : &targetA;
                auto* texture = frame % 2 ? &textureB : &textureA;
                const auto sourceState = frame % 2 ? render::BufferState::ShaderRead : render::BufferState::CopySource;
                const auto targetState = frame % 2 ? render::BufferState::Common : render::BufferState::CopyDestination;
                RenderExternalBuffer sourceInput{source, bufferDesc, sourceState, true};
                RenderExternalBuffer targetInput{target, bufferDesc, targetState, false};
                array<render::TextureStates, 4> states;
                for (uint32_t cell = 0; cell < states.size(); ++cell)
                    states[cell] = (frame + cell) % 2 ? render::TextureState::UnorderedAccess : render::TextureState::ShaderRead;
                const auto initialStates = states;
                array<uint8_t, 4> valid{1, 1, 1, 1};
                RenderExternalTexture textureInput{texture, textureDesc, states, valid};
                RenderGraph graph{device, resources, registry, "barrier instance"};
                graph.SetCompileOptions({.EliminateBarriers = eliminate, .ReuseCompiledPlan = reuse});
                const auto src = graph.ImportBuffer(sourceInput, "source", RenderGraphExternalAccess::ReadOnly);
                const auto dst = graph.NextVersion(graph.ImportBuffer(targetInput, "target", RenderGraphExternalAccess::ReadWrite));
                const auto tex = graph.ImportTexture(textureInput, "texture", RenderGraphExternalAccess::ReadOnly);
                graph.AddCopyBufferPass("copy range", src, dst, 16, 0, 16);
                graph.ExportBuffer(dst, RgBufferAccess::Vertex, {16, 16});
                graph.ExportTexture(tex, render::TextureState::UnorderedAccess);
                graph.ExportTexture(tex, render::TextureState::ShaderRead);
                test::UploadTestCommand native;
                test::FailingGraphCommand commands{native};
                const auto result = RenderGraphTestDriver::ExecuteWithPresent(graph, commands, {});
                ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
                EXPECT_EQ(native.Copies, 1u);
                EXPECT_EQ(targetInput.State, targetState);
                EXPECT_EQ(sourceInput.State, sourceState);
                EXPECT_EQ(states, initialStates);
                EXPECT_FALSE(targetInput.Written);
                EXPECT_EQ(graph.GetReport().InitialStatePatches, 6u);
                EXPECT_EQ(graph.GetReport().CommandRoutePatches, 4u);
                EXPECT_EQ(graph.GetReport().UavBarriers, eliminate ? 1u : 0u);
                EXPECT_EQ(graph.GetReport().TransitionBarriers, eliminate ? 9u : 11u);
                const auto words = BarrierWords(commands.RecordedBarriers);
                if (!reuse)
                    reference = words;
                else {
                    EXPECT_EQ(words, reference);
                    EXPECT_EQ(graph.GetReport().BarrierTemplateBuilds, frame == 0 ? 1u : 0u);
                    EXPECT_EQ(graph.GetReport().RoutePlanBuilds, frame == 0 ? 1u : 0u);
                    if (frame == 0)
                        identity = graph.GetReport().ExecutionPlanId;
                    else
                        EXPECT_EQ(graph.GetReport().ExecutionPlanId, identity);
                }
                ASSERT_TRUE(result.Submission);
                ASSERT_TRUE(result.Submission->Submit(frame + 1));
                EXPECT_EQ(sourceInput.State, render::BufferState::CopySource);
                EXPECT_EQ(targetInput.State, render::BufferState::Vertex);
                for (const auto state : states) EXPECT_EQ(state, render::TextureState::ShaderRead);
                EXPECT_TRUE(targetInput.Written);
                ASSERT_TRUE(result.Submission->Complete(frame + 1, true));
            }
        }
    }
}

TEST(RenderGraphPlanExecutionTest, PresentationRoutesUseCurrentTargetsAndRejectSplitBeforeRecording) {
    test::UploadTestDevice device;
    render::RenderPassRegistry registry{&device};
    RenderGraphFrameResources resources{device, registry};
    HostWriteBatch writes;
    auto desc = GraphColor();
    desc.Usage |= render::TextureUse::CopyDestination;
    desc.Hints |= render::ResourceHint::External;
    test::UploadTestTexture first{desc, device.LiveTextures}, second{desc, device.LiveTextures};
    uint64_t identity = 0;
    for (uint32_t frame = 0; frame < 6; ++frame) {
        resources.BeginFlight(frame + 1, writes);
        auto* left = frame % 2 ? &second : &first;
        auto* right = frame % 2 ? &first : &second;
        array<render::TextureStates, 1> leftStates{render::TextureState::ShaderRead}, rightStates{render::TextureState::CopySource};
        array<uint8_t, 1> valid{1};
        RenderExternalTexture leftInput{left, desc, leftStates, valid}, rightInput{right, desc, rightStates, valid};
        RenderGraph graph{device, resources, registry, "presentation patch"};
        const auto a = graph.ImportTexture(leftInput, "left", RenderGraphExternalAccess::ReadWrite);
        const auto b = graph.ImportTexture(rightInput, "right", RenderGraphExternalAccess::ReadWrite);
        graph.ExportTexture(a, render::TextureState::Present);
        graph.ExportTexture(b, render::TextureState::Present);
        test::UploadTestCommand defaultNative, leftNative, rightNative;
        test::FailingGraphCommand fallback{defaultNative}, leftCommands{leftNative}, rightCommands{rightNative};
        const std::pair<render::Texture*, render::CommandBuffer*> targets[]{{left, &leftCommands}, {right, &rightCommands}};
        const auto result = RenderGraphTestDriver::ExecuteWithPresent(graph, fallback, targets);
        ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
        EXPECT_TRUE(fallback.RecordedBarriers.empty());
        ASSERT_EQ(leftCommands.RecordedBarriers.size(), 1u);
        ASSERT_EQ(rightCommands.RecordedBarriers.size(), 1u);
        const auto* leftBarrier = std::get_if<render::BarrierTextureDescriptor>(&leftCommands.RecordedBarriers.front());
        const auto* rightBarrier = std::get_if<render::BarrierTextureDescriptor>(&rightCommands.RecordedBarriers.front());
        ASSERT_NE(leftBarrier, nullptr);
        ASSERT_NE(rightBarrier, nullptr);
        EXPECT_EQ(leftBarrier->Target, left);
        EXPECT_EQ(rightBarrier->Target, right);
        EXPECT_EQ(leftBarrier->Before, render::TextureState::ShaderRead);
        EXPECT_EQ(rightBarrier->Before, render::TextureState::CopySource);
        EXPECT_EQ(leftBarrier->After, render::TextureState::Present);
        EXPECT_EQ(rightBarrier->After, render::TextureState::Present);
        EXPECT_EQ(graph.GetReport().CommandRoutePatches, 2u);
        if (frame == 0)
            identity = graph.GetReport().ExecutionPlanId;
        else {
            EXPECT_EQ(graph.GetReport().ExecutionPlanId, identity);
            EXPECT_EQ(graph.GetReport().RoutePlanBuilds, 0u);
        }
        EXPECT_EQ(leftStates[0], render::TextureState::ShaderRead);
        ASSERT_TRUE(result.Submission->Submit(frame + 1));
        EXPECT_EQ(leftStates[0], render::TextureState::Present);
        EXPECT_EQ(rightStates[0], render::TextureState::Present);
        ASSERT_TRUE(result.Submission->Complete(frame + 1, true));
    }
    for (const bool sameTexture : {false, true}) {
        resources.BeginFlight(100, writes);
        array<render::TextureStates, 1> sourceStates{render::TextureState::CopySource}, targetStates{render::TextureState::CopyDestination};
        array<uint8_t, 1> valid{1};
        RenderExternalTexture sourceInput{&first, desc, sourceStates, valid}, targetInput{&second, desc, targetStates, valid};
        RenderGraph graph{device, resources, registry, "split route"};
        const auto source = graph.ImportTexture(sourceInput, "source", RenderGraphExternalAccess::ReadOnly);
        const auto target = graph.NextVersion(graph.ImportTexture(targetInput, "target", RenderGraphExternalAccess::ReadWrite));
        const auto copy = graph.AddCopyTexturePass("split copy", source, target);
        const auto ticket = graph.Track(copy);
        graph.ExportTexture(target, render::TextureState::Present);
        test::UploadTestCommand firstNative, secondNative;
        test::FailingGraphCommand firstCommands{firstNative}, secondCommands{secondNative};
        // One explicit presentation target may use the fallback command itself; it still owns a route.
        const std::pair<render::Texture*, render::CommandBuffer*> targets[]{{&first, &firstCommands}, {sameTexture ? &first : &second, &secondCommands}};
        const auto result = RenderGraphTestDriver::ExecuteWithPresent(graph, firstCommands, targets);
        EXPECT_FALSE(result.Success);
        EXPECT_FALSE(result.CommandsRecorded);
        EXPECT_EQ(graph.GetFirstErrorCode(), "PresentCommandSplit");
        EXPECT_EQ(ticket.Status(), FrameOperationStatus::Cancelled);
        EXPECT_TRUE(firstCommands.RecordedBarriers.empty());
        EXPECT_TRUE(secondCommands.RecordedBarriers.empty());
        EXPECT_EQ(firstNative.Copies + secondNative.Copies, 0u);
        EXPECT_EQ(targetStates[0], render::TextureState::CopyDestination);
    }
}

struct UploadWorkProbe {
    RgUploadData Upload;
    array<byte, 16> Bytes{};
    uint32_t Calls{0};
    uint64_t Mask{0};
    bool Fail{false};
    bool WrongSize{false};
};
bool PrepareUploadWork(shared_ptr<UploadWorkProbe>& probe, uint64_t mask) {
    ++probe->Calls;
    probe->Mask = mask;
    if (probe->Fail) return false;
    probe->Upload.Bytes = std::span<const byte>{probe->Bytes}.first(probe->WrongSize ? 15 : 16);
    return true;
}

TEST(RenderGraphLiveWorkTest, UnionRunsOnceAfterCompileAndReusesPlanWithNewFlightPayload) {
    test::UploadTestDevice device;
    render::RenderPassRegistry registry{&device};
    auto plans = make_shared<RenderGraphPlanCache>(2);
    array<unique_ptr<RenderGraphFrameResources>, 3> resources;
    array<HostWriteBatch, 3> writes;
    for (auto& flight : resources) flight = make_unique<RenderGraphFrameResources>(device, registry, plans);
    uint64_t identity = 0;
    for (uint32_t frame = 0; frame < 9; ++frame) {
        const auto flight = frame % resources.size();
        resources[flight]->BeginFlight(frame + 1, writes[flight]);
        auto live = make_shared<UploadWorkProbe>();
        auto dead = make_shared<UploadWorkProbe>();
        live->Bytes.fill(byte(frame + 1));
        dead->Fail = true;
        weak_ptr<UploadWorkProbe> retained = live;
        RenderGraphExecutionResult result;
        {
            RenderGraph graph{device, *resources[flight], registry, "typed uploads"};
            const auto shared = graph.AddWork("shared", live, PrepareUploadWork);
            const auto culled = graph.AddWork("culled", dead, PrepareUploadWork);
            const auto a = graph.UploadBuffer("left", 16, render::BufferUse::Resource, live->Upload, shared, 1);
            const auto b = graph.UploadBuffer("right", 16, render::BufferUse::Resource, live->Upload, shared, 2);
            graph.UploadBuffer("unused shared role", 16, render::BufferUse::Resource, live->Upload, shared, 4);
            graph.UploadBuffer("unused branch", 16, render::BufferUse::Resource, dead->Upload, culled, 8);
            graph.ExportBuffer(a, RgBufferAccess::ShaderRead);
            graph.ExportBuffer(b, RgBufferAccess::ShaderRead);
            ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
            EXPECT_EQ(live->Calls + dead->Calls, 0u);
            EXPECT_TRUE(live->Upload.Bytes.empty());
            EXPECT_EQ(graph.GetReport().DeclaredWorks, 2u);
            EXPECT_EQ(graph.GetReport().LiveWorks, 1u);
            if (frame == 0)
                identity = graph.GetReport().ExecutionPlanId;
            else {
                EXPECT_TRUE(graph.GetReport().CompilePlanReused);
                EXPECT_EQ(graph.GetReport().ExecutionPlanId, identity);
                EXPECT_EQ(graph.GetReport().NormalizeBuilds + graph.GetReport().IrBuilds + graph.GetReport().ExecutionPlanBuilds, 0u);
            }
            test::UploadTestCommand native;
            test::FailingGraphCommand commands{native};
            result = RenderGraphTestDriver::ExecuteWithPresent(graph, commands, {});
            ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
            EXPECT_EQ(live->Calls, 1u);
            EXPECT_EQ(live->Mask, 3u);
            EXPECT_EQ(dead->Calls, 0u);
            EXPECT_TRUE(dead->Upload.Bytes.empty());
            EXPECT_EQ(graph.GetReport().WorkRuns, 1u);
            EXPECT_EQ(graph.GetReport().WorkUploads, 2u);
            EXPECT_EQ(graph.GetReport().WorkUploadBytes, 32u);
            uint32_t inspected = 0;
            for (const auto& barrier : commands.RecordedBarriers) {
                const auto* buffer = std::get_if<render::BarrierBufferDescriptor>(&barrier);
                if (!buffer) continue;
                auto* bytes = static_cast<byte*>(buffer->Target->Map(0, 16));
                EXPECT_TRUE(std::equal(live->Bytes.begin(), live->Bytes.end(), bytes));
                buffer->Target->Unmap();
                ++inspected;
            }
            EXPECT_GT(inspected, 0u);
            EXPECT_EQ(native.Copies, 0u);
            live.reset();
        }
        EXPECT_FALSE(retained.expired());
        ASSERT_TRUE(result.Submission->Submit(frame + 1));
        ASSERT_TRUE(result.Submission->Complete(frame + 1, true));
        result.Submission.reset();
        EXPECT_TRUE(retained.expired());
    }
}

TEST(RenderGraphLiveWorkTest, TemplateSlotsPatchLiveUploadsAndRetainFramesAcrossCacheEviction) {
    test::UploadTestDevice device;
    render::RenderPassRegistry registry{&device};
    RenderResourcePool builderPool{device, registry};
    builderPool.BeginFlight(1);
    RenderGraph builder{device, builderPool, registry, "upload template"};
    const auto frameSlot = builder.DeclareTemplateSlot<UploadWorkProbe>();
    const auto uploadSlot = builder.DeclareTemplateSlot<RgUploadData>();
    const auto deadSlot = builder.DeclareTemplateSlot<UploadWorkProbe>();
    const auto prepare = +[](UploadWorkProbe& probe, uint64_t mask) {
        ++probe.Calls;
        probe.Mask = mask;
        probe.Upload.Bytes = probe.Bytes;
        return !probe.Fail;
    };
    const auto live = builder.AddTemplateWork("live", frameSlot, prepare);
    const auto dead = builder.AddTemplateWork("dead", deadSlot, prepare);
    const auto left = builder.UploadBuffer("left", 16, render::BufferUse::Resource, uploadSlot, live, 1);
    const auto right = builder.UploadBuffer("right", 16, render::BufferUse::Resource, uploadSlot, live, 2);
    builder.UploadBuffer("culled shared role", 16, render::BufferUse::Resource, uploadSlot, live, 4);
    builder.UploadBuffer("culled unbound work", 16, render::BufferUse::Resource, uploadSlot, dead, 8);
    builder.ExportBuffer(left, RgBufferAccess::ShaderRead);
    builder.ExportBuffer(right, RgBufferAccess::ShaderRead);
    auto graphTemplate = builder.FreezeTemplate();
    ASSERT_TRUE(graphTemplate) << builder.GetReport().ToText();
    auto plans = make_shared<RenderGraphPlanCache>(2);
    array<unique_ptr<RenderGraphFrameResources>, 3> flights;
    array<HostWriteBatch, 3> writes;
    for (auto& flight : flights) flight = make_unique<RenderGraphFrameResources>(device, registry, plans);
    uint64_t identity = 0;
    for (uint32_t frame = 0; frame < 6; ++frame) {
        const auto flight = frame % 3;
        flights[flight]->BeginFlight(frame + 1, writes[flight]);
        auto probe = make_shared<UploadWorkProbe>();
        probe->Bytes.fill(byte(frame + 1));
        weak_ptr<UploadWorkProbe> retained = probe;
        RenderGraphExecutionResult result;
        {
            RenderGraph graph{device, *flights[flight], registry, "template upload frame"};
            const auto instance = graph.Instantiate(graphTemplate);
            ASSERT_TRUE(instance.Bind(frameSlot, probe));
            ASSERT_TRUE(instance.Bind(uploadSlot, shared_ptr<RgUploadData>{probe, &probe->Upload}));
            const auto bytes = std::span<const byte>{probe->Bytes};
            const auto dynamic = graph.UploadBuffer("dynamic upload", bytes, render::BufferUse::Resource);
            graph.ExportBuffer(dynamic, RgBufferAccess::ShaderRead);
            ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
            EXPECT_EQ(probe->Calls, 0u);
            EXPECT_EQ(graph.GetReport().LiveWorks, 1u);
            if (frame == 0)
                identity = graph.GetReport().ExecutionPlanId;
            else {
                EXPECT_EQ(graph.GetReport().ExecutionPlanId, identity);
                EXPECT_EQ(graph.GetReport().PortResolveBuilds + graph.GetReport().TemplateMaterializations + graph.GetReport().TemplatePlacementBuilds + graph.GetReport().NormalizeBuilds, 0u);
            }
            test::UploadTestCommand native;
            test::FailingGraphCommand command{native};
            result = RenderGraphTestDriver::ExecuteWithPresent(graph, command, {});
            ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
            EXPECT_EQ(probe->Calls, 1u);
            EXPECT_EQ(probe->Mask, 3u);
            EXPECT_EQ(graph.GetReport().WorkUploads, 2u);
            EXPECT_EQ(graph.GetReport().WorkUploadBytes, 32u);
            uint32_t inspected = 0;
            for (const auto& barrier : command.RecordedBarriers) {
                const auto* buffer = std::get_if<render::BarrierBufferDescriptor>(&barrier);
                if (!buffer) continue;
                auto* actual = static_cast<byte*>(buffer->Target->Map(0, 16));
                EXPECT_TRUE(std::equal(probe->Bytes.begin(), probe->Bytes.end(), actual));
                buffer->Target->Unmap();
                ++inspected;
            }
            EXPECT_GT(inspected, 0u);
            probe.reset();
            if (frame == 5) {
                plans->Clear();
                graphTemplate.reset();
            }
        }
        EXPECT_FALSE(retained.expired());
        ASSERT_TRUE(result.Submission->Submit(frame + 1));
        ASSERT_TRUE(result.Submission->Complete(frame + 1, true));
        result.Submission.reset();
        EXPECT_TRUE(retained.expired());
    }
}

TEST(RenderGraphLiveWorkTest, PreparationFailureAndInvalidUploadCancelBeforeAnyRecording) {
    test::UploadTestDevice device;
    render::RenderPassRegistry registry{&device};
    RenderGraphFrameResources resources{device, registry};
    HostWriteBatch writes;
    uint64_t identity = 0;
    for (uint32_t frame = 0; frame < 3; ++frame) {
        resources.BeginFlight(frame + 1, writes);
        RenderGraph graph{device, resources, registry, "work failure retry"};
        auto probe = make_shared<UploadWorkProbe>();
        probe->Fail = frame == 0;
        probe->WrongSize = frame == 1;
        const auto work = graph.AddWork("producer", probe, PrepareUploadWork);
        const auto upload = graph.UploadBuffer("upload", 16, render::BufferUse::Resource, probe->Upload, work);
        const auto ticket = graph.ExportBuffer(upload, RgBufferAccess::ShaderRead);
        test::UploadTestCommand native;
        test::FailingGraphCommand commands{native};
        const auto result = RenderGraphTestDriver::ExecuteWithPresent(graph, commands, {});
        if (frame == 0)
            identity = graph.GetReport().ExecutionPlanId;
        else
            EXPECT_EQ(graph.GetReport().ExecutionPlanId, identity);
        EXPECT_EQ(probe->Calls, 1u);
        if (frame < 2) {
            EXPECT_FALSE(result.Success);
            EXPECT_FALSE(result.CommandsRecorded);
            EXPECT_EQ(graph.GetFirstErrorCode(), frame == 0 ? "WorkPreparation" : "UploadData");
            EXPECT_EQ(ticket.Status(), FrameOperationStatus::Cancelled);
            EXPECT_TRUE(commands.RecordedBarriers.empty());
            EXPECT_EQ(native.Copies, 0u);
        } else {
            ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
            ASSERT_TRUE(result.Submission->Submit(frame + 1));
            ASSERT_TRUE(result.Submission->Complete(frame + 1, true));
            EXPECT_EQ(ticket.Status(), FrameOperationStatus::GpuCompleted);
        }
    }
}

TEST_F(RenderGraphCompileTest, WorkHandleGenerationAndMaskAreValidatedBeforeCompilation) {
    auto first = MakeGraph();
    auto probe = make_shared<UploadWorkProbe>();
    const auto old = first.AddWork("old", probe, PrepareUploadWork);
    for (bool stale : {false, true}) {
        auto graph = MakeGraph();
        const auto current = graph.AddWork("current", probe, PrepareUploadWork);
        graph.AddComputePass<EmptyPass>("invalid work", [&](EmptyPass&, RenderGraphComputeBuilder& builder) {
            builder.RequireWork(stale ? old : current, stale ? 1 : 0);
            builder.SetSideEffect(); }, EmptyCompute);
        EXPECT_FALSE(graph.Compile());
        EXPECT_EQ(graph.GetFirstErrorCode(), "WorkHandle");
        EXPECT_EQ(probe->Calls, 0u);
        EXPECT_EQ(Device.NativeCreates, 0u);
    }
}

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
    auto texture = graph.CreateTexture(GraphColor(2), "mips");
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
        const auto target = graph.NextVersion(graph.ImportTexture(external, "observable output", RenderGraphExternalAccess::ObservableOutput));
        array<RgTextureValue, 3> ends;
        for (uint32_t chain = 0; chain < 3; ++chain) {
            RgTextureValue previous;
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
                const auto target = graph.NextVersion(graph.ImportTexture(external, "observable final content", RenderGraphExternalAccess::ObservableOutput));
                vector<RgTextureValue> textures;
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
                auto structuralReport = report;
                structuralReport.ExecutionPlanId = 0;  // Cold compilations have distinct identities; their complete structure must still match.
                if (iteration == 0) {
                    json = structuralReport.ToJson();
                    dot = report.ToDot();
                    reportBytes = report.Passes.capacity() * sizeof(RenderGraphPassReport) + report.Resources.capacity() * sizeof(RenderGraphResourceReport);
                    reportBlocks = 2;
                    for (const auto& pass : report.Passes) {
                        reportBytes += (pass.DataDependencies.capacity() + pass.HazardDependencies.capacity()) * sizeof(uint32_t);
                        reportBlocks += !pass.DataDependencies.empty();
                        reportBlocks += !pass.HazardDependencies.empty();
                    }
                } else {
                    ASSERT_EQ(structuralReport.ToJson(), json);
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

TEST_F(RenderGraphCompileTest, PortsConnectConsumerDeclaredBeforeProducer) {
    auto graph = MakeGraph("component ports");
    auto input = graph.DeclareTexturePort(GraphColor(), "consumer input");
    const auto output = graph.CreateTexture(GraphColor(), "output");
    graph.AddRasterPass<EmptyPass>("consumer", [&](EmptyPass&, RenderGraphRasterBuilder& builder) {
        builder.ReadTexture(graph.Value(input));
        builder.SetColorAttachment(0, output);
        builder.SetSideEffect(); }, EmptyRaster);
    auto source = graph.CreateTexture(GraphColor(), "source");
    Clear(graph, source, "producer");
    ASSERT_TRUE(graph.Connect(input, source));
    ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
    EXPECT_EQ(graph.GetCompiledGraph().ExecutionOrder, (vector<uint32_t>{1, 0}));
    EXPECT_NE(graph.GetReport().ToDot().find("shape=ellipse"), string::npos);
}

TEST_F(RenderGraphCompileTest, PortsRejectMissingConnectionsCyclesAndBranches) {
    {
        auto graph = MakeGraph();
        graph.DeclareTexturePort(GraphColor(), "missing");
        EXPECT_FALSE(graph.Compile());
        EXPECT_EQ(graph.GetReport().Diagnostics.front().Code, "UnconnectedPort");
    }
    {
        auto graph = MakeGraph();
        const auto a = graph.DeclareTexturePort(GraphColor(), "a");
        const auto b = graph.DeclareTexturePort(GraphColor(), "b");
        ASSERT_TRUE(graph.Connect(a, graph.Value(b)));
        ASSERT_TRUE(graph.Connect(b, graph.Value(a)));
        EXPECT_FALSE(graph.Compile());
        EXPECT_EQ(graph.GetReport().Diagnostics.front().Code, "PortCycle");
    }
    {
        auto graph = MakeGraph();
        const auto a = graph.CreateTexture(GraphColor(), "a");
        graph.NextVersion(a);
        EXPECT_FALSE(graph.NextVersion(a).IsValid());
        EXPECT_FALSE(graph.Compile());
    }
}

TEST_F(RenderGraphCompileTest, CompatibleStorageReusesOnlyDisjointFinalLifetimes) {
    for (const bool reuse : {false, true}) {
        auto graph = MakeGraph();
        graph.SetCompileOptions({.ReuseResources = reuse});
        auto a = graph.CreateTexture(GraphColor(), "a");
        auto b = graph.CreateTexture(GraphColor(), "b");
        Clear(graph, a, "a", render::LoadAction::Clear, render::StoreAction::Store, true);
        Clear(graph, b, "b", render::LoadAction::Clear, render::StoreAction::Store, true);
        ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
        EXPECT_EQ(graph.GetReport().ReusedResources, reuse ? 1u : 0u);
        EXPECT_EQ(graph.GetReport().Resources[0].PhysicalSlot == graph.GetReport().Resources[1].PhysicalSlot, reuse);
    }
}

TEST_F(RenderGraphCompileTest, PartialBufferConsumersNeedOnlyTheirDeclaredBytes) {
    auto graph = MakeGraph();
    const auto buffer = graph.CreateBuffer({64, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::Resource, {}}, "partial");
    graph.AddComputePass<EmptyPass>("writer", [=](EmptyPass&, RenderGraphComputeBuilder& builder) { builder.WriteBuffer(buffer, RgBufferAccess::UnorderedAccess, {16, 16}); }, EmptyCompute);
    graph.AddComputePass<EmptyPass>("reader", [=](EmptyPass&, RenderGraphComputeBuilder& builder) { builder.ReadBuffer(buffer, RgBufferAccess::ShaderRead, {16, 16}); builder.SetSideEffect(); }, EmptyCompute);
    ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
    EXPECT_EQ(graph.GetReport().Passes[1].DataDependencies, (vector<uint32_t>{0}));
}

TEST(FrameSubmissionTest, FrameSerialGuardsSubmitCompletionAndCancellation) {
    FrameSubmission receipt{42};
    uint32_t submitted = 0, completed = 0;
    receipt.OnSubmitted = [&] { ++submitted; };
    receipt.OnCompleted = [&](bool success) { if (success) ++completed; };
    EXPECT_FALSE(receipt.Complete(42, true));
    EXPECT_TRUE(receipt.Record());
    EXPECT_FALSE(receipt.Submit(43));
    EXPECT_TRUE(receipt.Submit(42));
    EXPECT_FALSE(receipt.Submit(42));
    EXPECT_FALSE(receipt.Complete(43, true));
    EXPECT_TRUE(receipt.Complete(42, true));
    EXPECT_EQ(receipt.Status(), FrameOperationStatus::GpuCompleted);
    EXPECT_EQ(submitted, 1u);
    EXPECT_EQ(completed, 1u);
    FrameSubmission cancelled{44};
    cancelled.Record();
    cancelled.Cancel();
    EXPECT_FALSE(cancelled.Submit(44));
    EXPECT_EQ(cancelled.Status(), FrameOperationStatus::Cancelled);
}

TEST_F(RenderGraphCompileTest, AspectValidityAndIllegalAspectViewsAreIndependent) {
    auto desc = GraphColor();
    desc.Format = render::TextureFormat::D24_UNORM_S8_UINT;
    desc.Usage = render::TextureUse::Resource | render::TextureUse::DepthStencilRead | render::TextureUse::DepthStencilWrite;
    CompileTexture native{desc};
    array<render::TextureStates, 1> states{render::TextureState::ShaderRead};
    array<uint8_t, 2> valid{1, 0};
    RenderExternalTexture external{&native, desc, states, valid};
    for (const auto aspect : {render::TextureAspect::Depth, render::TextureAspect::Stencil, render::TextureAspect::Color}) {
        auto graph = MakeGraph();
        const auto value = graph.ImportTexture(external, "depth-stencil", RenderGraphExternalAccess::ReadOnly);
        graph.AddComputePass<EmptyPass>("read one aspect", [=](EmptyPass&, RenderGraphComputeBuilder& b) {
            b.ReadTexture(value, {.Range = {0, 1, 0, 1, aspect}}); b.SetSideEffect(); }, EmptyCompute);
        EXPECT_EQ(graph.Compile(), aspect == render::TextureAspect::Depth) << graph.GetReport().ToText();
        if (aspect == render::TextureAspect::Depth) {
            ASSERT_EQ(graph.GetReport().Passes[0].Reads.size(), 1u);
            const auto& node = graph.GetCompiledGraph().Versions[graph.GetReport().Passes[0].Reads[0]];
            EXPECT_EQ(node.Cell, 0u);
        }
    }
}

TEST_F(RenderGraphCompileTest, CanonicalImportsRejectConflictsAndInvalidVersionPorts) {
    CompileTexture native{GraphColor()};
    array<render::TextureStates, 1> states{render::TextureState::ShaderRead};
    array<uint8_t, 1> valid{1};
    RenderExternalTexture first{&native, native.Desc, states, valid}, second{&native, native.Desc, states, valid};
    {
        auto graph = MakeGraph();
        const auto a = graph.ImportTexture(first, "a", RenderGraphExternalAccess::ReadOnly);
        const auto b = graph.ImportTexture(second, "b", RenderGraphExternalAccess::ReadOnly);
        EXPECT_EQ(a, b);
        graph.ExportTexture(b, render::TextureState::ShaderRead);
        EXPECT_TRUE(graph.Compile()) << graph.GetReport().ToText();
        EXPECT_EQ(graph.GetReport().Resources.size(), 1u);
    }
    {
        auto graph = MakeGraph();
        graph.ImportTexture(first, "a", RenderGraphExternalAccess::ReadOnly);
        array<uint8_t, 1> invalid{0};
        second.ContentValid = invalid;
        EXPECT_FALSE(graph.ImportTexture(second, "conflict", RenderGraphExternalAccess::ReadOnly).IsValid());
        EXPECT_FALSE(graph.Compile());
    }
    {
        auto graph = MakeGraph();
        const auto port = graph.DeclareTexturePort(GraphColor(), "port");
        auto bad = graph.Value(port);
        bad.Version = 999;
        graph.AddComputePass<EmptyPass>("bad value", [=](EmptyPass&, RenderGraphComputeBuilder& b) { b.ReadTexture(bad); }, EmptyCompute);
        auto source = graph.CreateTexture(GraphColor(), "source");
        Clear(graph, source);
        ASSERT_TRUE(graph.Connect(port, source));
        EXPECT_FALSE(graph.Compile());
        EXPECT_EQ(graph.GetReport().Diagnostics.front().Code, "InvalidVersion");
    }
}

TEST_F(RenderGraphCompileTest, RasterMergingAndDeadStoresHaveIndependentSwitches) {
    for (const bool merge : {false, true})
        for (const bool discard : {false, true}) {
            auto graph = MakeGraph();
            graph.SetCompileOptions({.MergeRasterPasses = merge, .OptimizeAttachmentStores = discard});
            auto color = graph.CreateTexture(GraphColor(), "color");
            Clear(graph, color, "clear");
            Clear(graph, color, "load", render::LoadAction::Load, render::StoreAction::Store, true);
            ASSERT_TRUE(graph.Compile()) << graph.GetReport().ToText();
            EXPECT_EQ(graph.GetReport().MergedRasterPasses, merge ? 1u : 0u);
            EXPECT_EQ(graph.GetReport().DiscardedStores, discard ? 1u : 0u);
            EXPECT_EQ(graph.GetReport().Passes[1].RasterGroup, merge ? 0u : 1u);
        }
}

TEST(FrameSubmissionTest, AbandonedRecordingCancelsDependentOperations) {
    auto operation = make_shared<FrameSubmission>(8);
    operation->Record();
    {
        FrameSubmission recording{8};
        recording.Record();
        recording.OnCompleted = [operation](bool) { operation->Cancel(); };
    }
    EXPECT_EQ(operation->Status(), FrameOperationStatus::Cancelled);
}

}  // namespace
}  // namespace radray
