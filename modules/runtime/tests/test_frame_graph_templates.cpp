#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include "render_graph_test_driver.h"

#include <algorithm>

#include <radray/runtime/render_framework/render_graph_blit.h>
#include <radray/runtime/render_framework/render_graph_runtime.h>
#include <radray/runtime/render_system.h>

#include <gtest/gtest.h>

namespace radray {
namespace {

struct ComponentTrace {
    vector<uint32_t> BuildOrder, RecordOrder;
    uint32_t Payload{0};
};

class ProbePipeline final : public RenderPipeline {
public:
    ProbePipeline(ComponentTrace& trace, uint32_t id, bool dynamic) : Trace(trace), Id(id), Dynamic(dynamic) {}
    void BuildGraph(RenderPipelineContext&, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) override {
        Trace.BuildOrder.push_back(Id);
        if (!Dynamic) return;
        struct Data {
            ComponentTrace* Trace;
            uint32_t Id, Payload;
        };
        for (auto& output : outputs) {
            output.Texture = graph.NextVersion(output.Texture);
            graph.AddRasterPass<Data>("dynamic component", [&](Data& data, RenderGraphRasterBuilder& builder) {
                data = {&Trace, Id, Trace.Payload};
                builder.SetColorAttachment(0, output.Texture, {.Load = Id == 0 ? render::LoadAction::Clear : render::LoadAction::Load, .Clear = {{.25f, .5f, .75f, 1}}}); }, +[](const Data& data, RenderGraphRasterContext&) {
                EXPECT_EQ(data.Payload, data.Trace->Payload);
                data.Trace->RecordOrder.push_back(data.Id); });
        }
    }
    ComponentTrace& Trace;
    uint32_t Id;
    bool Dynamic;
};

enum class CompositionScenario { WarmDeclarations,
                                 Variants,
                                 NativeBlits };

class CompositionTemplateApp final : public Application {
public:
    explicit CompositionTemplateApp(CompositionScenario scenario) : Scenario(scenario) {}
    uint32_t Verified{0};

private:
    void OnInit() override {
        auto* device = GetDevice();
        render::RenderPassRegistry registry{device};
        ViewStateRegistry views{*device, registry, 3};
        auto plans = make_shared<RenderGraphPlanCache>();
        array<unique_ptr<RenderGraphFrameResources>, 3> resources;
        array<HostWriteBatch, 3> writes;
        for (auto& flight : resources) flight = make_unique<RenderGraphFrameResources>(*device, registry, plans);
        vector<render::test::RenderTarget> targets;
        for (uint32_t index = 0; index < 6; ++index) {
            const auto format = Scenario == CompositionScenario::NativeBlits && index >= 3 ? render::TextureFormat::RGBA8_UNORM_SRGB : render::TextureFormat::RGBA8_UNORM;
            auto target = render::test::MakeRenderTarget(device, format, index < 3 ? 16 : 24, 16,
                                                         render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource);
            ASSERT_TRUE(target);
            targets.push_back(std::move(*target));
        }
        if (Scenario == CompositionScenario::WarmDeclarations) {
            WarmDeclarations(registry, views, resources, writes, targets);
        } else if (Scenario == CompositionScenario::Variants) {
            Variants(registry, views, resources, writes, targets);
        } else {
            NativeBlits(registry, views, resources, writes, targets);
        }
        for (auto& flight : resources) flight.reset();
        for (auto& target : targets) registry.RemoveFramebuffersUsing(target.View.get());
    }
    void OnUpdate(const AppUpdateContext&) override { test::CloseMainWindow(*this); }

    using Flights = array<unique_ptr<RenderGraphFrameResources>, 3>;
    static RenderSurfaceFrame Surface(render::test::RenderTarget& target, uint64_t id = 1, bool preserve = false) {
        return {{id}, target.Tex.get(), target.View.get(), target.Tex->GetDesc(), render::TextureState::Undefined, render::TextureState::ShaderRead, preserve};
    }
    void WarmDeclarations(render::RenderPassRegistry& registry, ViewStateRegistry& views, Flights& resources,
                          array<HostWriteBatch, 3>& writes, vector<render::test::RenderTarget>& targets) {
        ComponentTrace trace;
        ProbePipeline pipeline{trace, 0, false};
        array<uint64_t, 2> planIds{};
        for (uint32_t index = 0; index < 1000; ++index) {
            const auto flight = index % 3;
            const auto variant = (index / 3) % 2;
            resources[flight]->BeginFlight(index + 1, writes[flight]);
            AppFrameContext appFrame{GetGpuSystem(), flight, {}, {}, false};
            auto surface = Surface(targets[variant * 3 + flight]);
            RenderGraphExecutionReport report;
            RenderPipelineContext context{appFrame, *resources[flight], registry, views, index + 1, {}, std::span{&surface, 1}, report};
            auto graph = context.CreateRenderGraph("default composer warm A/B");
            FrameGraph frame{context, graph};
            ComposeDefaultFrameGraph(frame, &pipeline);
            ASSERT_TRUE(frame.Expand());
            ASSERT_TRUE(graph.Compile()) << report.ToText();
            EXPECT_EQ(report.ResourceDeclarations, 0u);
            EXPECT_EQ(report.PassDeclarations, 0u);
            EXPECT_EQ(report.TemplateInstances, 1u);
            EXPECT_EQ(report.LivePasses, 2u);
            EXPECT_EQ(std::count_if(report.Passes.begin(), report.Passes.end(), [](const auto& pass) { return pass.Live && pass.Type == RgPassType::Raster; }), 1);
            EXPECT_EQ(std::count_if(report.Passes.begin(), report.Passes.end(), [](const auto& pass) { return pass.Live && pass.Type == RgPassType::Export; }), 1);
            if (planIds[variant] != 0) {
                EXPECT_TRUE(report.CompilePlanReused);
                EXPECT_EQ(report.ExecutionPlanId, planIds[variant]);
                EXPECT_EQ(report.NormalizeBuilds, 0u);
                EXPECT_EQ(report.PortResolveBuilds, 0u);
                EXPECT_EQ(report.TemplateMaterializations, 0u);
            } else {
                planIds[variant] = report.ExecutionPlanId;
            }
            EXPECT_LE(context.GetDefaultCompositionCache().Size(), 2u);
            EXPECT_LE(context.GetDefaultCompositionCache().BuildCount(), 2u);
            if (index == 999) EXPECT_EQ(context.GetDefaultCompositionCache().BuildCount(), 2u);
            ++Verified;
        }
        EXPECT_EQ(trace.BuildOrder, vector<uint32_t>(1000, 0));
    }

    void Variants(render::RenderPassRegistry& registry, ViewStateRegistry& views, Flights& resources,
                  array<HostWriteBatch, 3>& writes, vector<render::test::RenderTarget>& targets) {
        ComponentTrace trace;
        ProbePipeline pipeline{trace, 0, false}, first{trace, 1, false}, second{trace, 2, false};
        FrameGraphTemplateCache cache;
        const array<uint32_t, 13> variants{0, 1, 2, 3, 0, 4, 0, 5, 0, 6, 0, 7, 1};
        uint64_t expectedBuilds = 0;
        vector<uint32_t> expectedOrder;
        for (uint32_t index = 0; index < variants.size(); ++index) {
            const auto variant = variants[index];
            const auto flight = index % 3;
            resources[flight]->BeginFlight(index + 1, writes[flight]);
            AppFrameContext appFrame{GetGpuSystem(), flight, {}, {}, false};
            auto surface = Surface(targets[flight], variant == 1 ? 2 : 1, variant == 2);
            if (variant == 3) surface.RequiredFinalState = render::TextureState::CopySource;
            if (variant == 4) surface = Surface(targets[3 + flight]);
            if (variant == 2) surface.CurrentState = render::TextureState::ShaderRead;
            RenderGraphExecutionReport report;
            RenderPipelineContext context{appFrame, *resources[flight], registry, views, index + 1, {}, std::span{&surface, 1}, report};
            auto graph = context.CreateRenderGraph("default composer key variants");
            FrameGraph frame{context, graph};
            array<RenderGraphComponent*, 2> overlayOrder{&first, &second};
            const size_t overlayCount = variant == 6 ? 1 : variant == 7 ? 2
                                                                        : 0;
            const auto exported = cache.Compose(frame, variant == 5 ? Nullable<RenderPipeline*>{nullptr} : &pipeline,
                                                std::span{overlayOrder}.first(overlayCount), GetRenderSystem());
            ASSERT_EQ(exported.size(), 1u);
            EXPECT_EQ(exported.front().Output, surface.Id);
            ASSERT_TRUE(frame.Expand());
            ASSERT_TRUE(graph.Compile()) << report.ToText();
            if (variant != 5) expectedOrder.push_back(0);
            for (uint32_t overlay = 0; overlay < overlayCount; ++overlay) expectedOrder.push_back(overlay + 1);
            if (index == 0 || variant != 0) ++expectedBuilds;
            EXPECT_EQ(cache.BuildCount(), expectedBuilds);
            EXPECT_EQ(cache.Size(), std::min<size_t>(4, expectedBuilds));
            EXPECT_EQ(report.ResourceDeclarations, 0u);
            EXPECT_EQ(report.PassDeclarations, 0u);
            ++Verified;
        }
        EXPECT_EQ(trace.BuildOrder, expectedOrder);
        cache.Clear();
        EXPECT_EQ(cache.Size(), 0u);
        EXPECT_EQ(cache.BuildCount(), 0u);
    }

    void NativeBlits(render::RenderPassRegistry& registry, ViewStateRegistry& views, Flights& resources,
                     array<HostWriteBatch, 3>& writes, vector<render::test::RenderTarget>& targets) {
        ComponentTrace trace;
        ProbePipeline pipeline{trace, 0, true}, first{trace, 1, true}, second{trace, 2, true};
        auto queue = GetDevice()->GetCommandQueue(render::QueueType::Direct);
        ASSERT_TRUE(queue);
        array<render::TextureStates, 6> states;
        states.fill(render::TextureState::Undefined);
        for (uint32_t index = 0; index < 12; ++index) {
            const auto targetIndex = (index / 2) % 6;
            const auto flight = index % 3;
            const bool preserve = index % 2 != 0;
            trace.Payload = index;
            trace.BuildOrder.clear();
            trace.RecordOrder.clear();
            resources[flight]->BeginFlight(index + 1, writes[flight]);
            AppFrameContext appFrame{GetGpuSystem(), flight, {}, {}, false};
            auto surface = Surface(targets[targetIndex], 1, preserve);
            surface.CurrentState = states[targetIndex];
            RenderGraphExecutionReport report;
            RenderPipelineContext context{appFrame, *resources[flight], registry, views, index + 1, {}, std::span{&surface, 1}, report};
            auto graph = context.CreateRenderGraph("default composer native blits");
            FrameGraph frame{context, graph};
            array<RenderGraphComponent*, 2> overlays{&first, &second};
            if (targetIndex % 2) std::swap(overlays[0], overlays[1]);
            const auto exported = ComposeDefaultFrameGraph(frame, preserve ? Nullable<RenderPipeline*>{nullptr} : &pipeline, overlays, *GetRenderSystem());
            ASSERT_EQ(exported.size(), 1u);
            auto readback = graph.ReadbackTexture("composed display", exported.front().Texture);
            // The capture is a later CopySource consumer than the composition's export pass.
            graph.ExportTexture(exported.front().Texture, surface.RequiredFinalState);
            ASSERT_TRUE(frame.Expand());
            auto command = GetDevice()->CreateCommandBuffer(queue.Get());
            ASSERT_TRUE(command);
            command->Begin();
            const auto result = RenderGraphTestDriver::Execute(graph, *command);
            ASSERT_TRUE(result.Success) << report.ToText();
            writes[flight].Flush(*GetDevice());
            command->End();
            auto* raw = command.Get();
            queue->Submit({.CmdBuffers = std::span{&raw, 1}});
            RenderGraphTestDriver::Submitted(raw);
            queue->Wait();
            RenderGraphTestDriver::Completed(raw);
            frame.Recorded(result);
            const auto finalState = graph.RecordedTextureState(exported.front().Texture);
            ASSERT_TRUE(finalState);
            states[targetIndex] = *finalState;
            EXPECT_EQ(*finalState, render::TextureState::ShaderRead);
            vector<uint32_t> expected;
            if (!preserve) expected.push_back(0);
            expected.push_back(targetIndex % 2 ? 2 : 1);
            expected.push_back(targetIndex % 2 ? 1 : 2);
            EXPECT_EQ(trace.BuildOrder, expected);
            EXPECT_EQ(trace.RecordOrder, expected);
            EXPECT_EQ(report.GraphicsPipelinePreparations, preserve ? 2u : 1u);
            EXPECT_EQ(report.CommandCalls.Draw, preserve ? 2u : 1u);
            vector<byte> pixels;
            ASSERT_TRUE(readback.Read(pixels));
            ASSERT_GE(pixels.size(), 4u);
            EXPECT_NEAR(uint8_t(pixels[0]), 137, 2);
            EXPECT_NEAR(uint8_t(pixels[1]), 188, 2);
            EXPECT_NEAR(uint8_t(pixels[2]), 225, 2);
            EXPECT_EQ(uint8_t(pixels[3]), 255u);
            ++Verified;
        }
    }

    CompositionScenario Scenario;
};

class FrameGraphTemplateTest : public testing::TestWithParam<render::RenderBackend> {};

void RunComposition(render::RenderBackend backend, CompositionScenario scenario, uint32_t expected) {
    {
        render::test::DeviceContext probe;
        if (!render::test::TryCreateDevice(backend, probe, true)) {
            if (render::test::SetupMustFail(probe.Status, render::test::RequiredBackend(backend))) FAIL() << probe.Reason;
            GTEST_SKIP() << probe.Reason;
        }
    }
    test::RuntimeLogCapture logs;
    CompositionTemplateApp app{scenario};
    ASSERT_EQ(app.Run({.Backend = backend, .EnableValidation = true, .Multithreaded = false, .WindowTitle = "Default composition templates", .WindowWidth = 64, .WindowHeight = 48, .FlightDataCount = 3, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}), 0);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    EXPECT_EQ(app.Verified, expected);
}

TEST_P(FrameGraphTemplateTest, WarmABAcrossFlightsAndNativeTargetsDeclaresNothingForOneThousandFrames) {
    RunComposition(GetParam(), CompositionScenario::WarmDeclarations, 1000);
}
TEST_P(FrameGraphTemplateTest, StructuralOutputAndComponentVariantsUseBoundedLRUDeclarations) {
    RunComposition(GetParam(), CompositionScenario::Variants, 13);
}
TEST_P(FrameGraphTemplateTest, DynamicComponentsAndPreservedSrgbBlitsExecuteInCurrentRegistrationOrder) {
    RunComposition(GetParam(), CompositionScenario::NativeBlits, 12);
}
INSTANTIATE_TEST_SUITE_P(Backends, FrameGraphTemplateTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
