#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include <gtest/gtest.h>
#include <radray/runtime/render_system.h>
#include <radray/utility.h>

namespace radray {
namespace {
enum class OutputScenario { Zero,
                            External,
                            Multiple,
                            Skip,
                            Failure,
                            FinalRead };
struct OutputResult {
    uint32_t Rendered{0}, Available{0};
    uint32_t SideEffects{0};
    bool FailureObserved{false};
};
class OutputPipeline final : public RenderPipeline {
public:
    OutputPipeline(OutputScenario scenario, OutputResult& result) : Scenario(scenario), Result(result) {}
    void PrepareFrame(RenderPrepareContext& ctx) override {
        if (Scenario == OutputScenario::Zero) return;
        for (const auto& output : ctx.Outputs)
            if (output.Kind == RenderOutputKind::ExternalColorTexture) {
                EXPECT_TRUE(ctx.Workloads.AddViewFamily({output.Name, output.Id}));
            }
    }
    void Render(RenderPipelineContext& ctx) override {
        ++Result.Rendered;
        for (const auto& family : ctx.ViewFamilies())
            if (family.OutputAvailable) ++Result.Available;
        if (Scenario == OutputScenario::Skip) return;
        auto graph = ctx.CreateRenderGraph(Scenario == OutputScenario::Failure ? "ExpectedFailure" : "Offscreen");
        struct Data {};
        if (Scenario == OutputScenario::Zero) {
            struct SideEffectData {
                OutputResult* Result;
            };
            graph.AddComputePass<SideEffectData>("without outputs", [&](SideEffectData& data, RenderGraphComputeBuilder& builder) {
                data.Result = &Result; builder.SetSideEffect(); }, +[](const SideEffectData& data, RenderGraphComputeContext&) { ++data.Result->SideEffects; });
        }
        for (const auto& family : ctx.ViewFamilies()) {
            if (!family.OutputAvailable) continue;
            if (Scenario == OutputScenario::Multiple && family.FrameLocalIndex == 1) continue;
            const auto color = ctx.ImportOutput(graph, family.OutputId);
            EXPECT_EQ(ctx.ImportOutput(graph, family.OutputId), color);
            graph.AddRasterPass<Data>("output", [=, this](Data&, RenderGraphRasterBuilder& builder) {
                const auto load = Scenario == OutputScenario::Failure ? render::LoadAction::Load : render::LoadAction::Clear;
                const float red = family.FrameLocalIndex == 0 ? .75f : .25f;
                builder.SetColorAttachment(0, color, {.Load = load, .Clear = {{red, .5f, .25f, 1}}}); }, +[](const Data&, RenderGraphRasterContext&) {});
            if (Scenario == OutputScenario::FinalRead) graph.AddComputePass<Data>("leave shader read", [=](Data&, RenderGraphComputeBuilder& builder) {
                builder.ReadTexture(color); builder.SetSideEffect(); }, +[](const Data&, RenderGraphComputeContext&) {});
        }
        const auto result = ctx.ExecuteGraph(graph);
        if (Scenario == OutputScenario::Failure) {
            EXPECT_FALSE(result.Success);
            EXPECT_FALSE(result.CommandsRecorded);
            Result.FailureObserved = true;
        } else
            EXPECT_TRUE(result.Success);
        EXPECT_FALSE(ctx.ExecuteGraph(graph).Success);
    }

private:
    OutputScenario Scenario;
    OutputResult& Result;
};

class OutputApp final : public Application {
public:
    OutputApp(OutputScenario scenario, OutputResult& result) : Scenario(scenario), Result(result) {}

protected:
    void OnInit() override {
        // A native window still drives the test loop; it has no presentation workload.
        const uint32_t count = Scenario == OutputScenario::Zero ? 0 : Scenario == OutputScenario::Multiple ? 2
                                                                                                           : 1;
        for (uint32_t i = 0; i < count; ++i) {
            auto target = render::test::MakeRenderTarget(GetDevice(), render::TextureFormat::RGBA8_UNORM, 16 + i * 8, 16,
                                                         render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource);
            ASSERT_TRUE(target);
            auto id = GetRenderSystem()->GetOutputs().RegisterExternal({fmt::format("external {}", i), target->Tex.get(), target->View.get(),
                                                                        render::TextureState::Undefined, render::TextureState::ShaderRead, false});
            ASSERT_TRUE(id.IsValid());
            Ids.push_back(id);
            Targets.push_back(std::move(*target));
        }
        GetRenderSystem()->SetPipeline(make_unique<OutputPipeline>(Scenario, Result));
    }
    void OnUpdate(const AppUpdateContext&) override {
        if (++Updates >= 5) test::CloseMainWindow(*this);
    }
    void OnShutdown() override {
        for (size_t i = 0; i < Targets.size(); ++i) {
            auto surface = GetRenderSystem()->GetOutputs().ResolveExternal(Ids[i]);
            ASSERT_TRUE(surface);
            EXPECT_EQ(surface->CurrentState, render::TextureState::ShaderRead);
            auto& target = Targets[i];
            const auto desc = target.Tex->GetDesc();
            const uint64_t row = Align(uint64_t{desc.Width} * 4, GetDevice()->GetDetail().TextureDataPitchAlignment);
            auto readback = GetDevice()->CreateBuffer({row * desc.Height, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
            auto queue = GetDevice()->GetCommandQueue(render::QueueType::Direct);
            ASSERT_TRUE(queue);
            ASSERT_TRUE(readback);
            auto command = GetDevice()->CreateCommandBuffer(queue.Get());
            ASSERT_TRUE(command);
            command->Begin();
            const render::ResourceBarrierDescriptor toCopy = render::BarrierTextureDescriptor{.Target = target.Tex.get(), .Before = surface->CurrentState, .After = render::TextureState::CopySource};
            command->ResourceBarrier(std::span{&toCopy, 1});
            command->CopyTextureToBuffer(readback.Get(), 0, target.Tex.get(), {0, 1, 0, 1});
            const render::ResourceBarrierDescriptor host = render::BarrierBufferDescriptor{.Target = readback.Get(), .Before = render::BufferState::CopyDestination, .After = render::BufferState::HostRead};
            command->ResourceBarrier(std::span{&host, 1});
            command->End();
            auto* raw = command.Get();
            queue->Submit({.CmdBuffers = std::span{&raw, 1}});
            queue->Wait();
            auto* mapped = static_cast<const uint8_t*>(readback->Map(0, row * desc.Height));
            ASSERT_NE(mapped, nullptr);
            readback->InvalidateMappedRange({0, row * desc.Height});
            const bool fallback = Scenario == OutputScenario::Skip || Scenario == OutputScenario::Failure || (Scenario == OutputScenario::Multiple && i == 1);
            EXPECT_NEAR(mapped[0], fallback ? .08f * 255 : (i == 0 ? .75f : .25f) * 255, 1);
            EXPECT_NEAR(mapped[1], fallback ? .10f * 255 : .5f * 255, 1);
            EXPECT_NEAR(mapped[2], fallback ? .14f * 255 : .25f * 255, 1);
            EXPECT_EQ(mapped[3], 255u);
            readback->Unmap();
            EXPECT_TRUE(GetRenderSystem()->GetOutputs().Unregister(Ids[i]));
            GetRenderSystem()->GetRenderPassRegistry()->RemoveFramebuffersUsing(target.View.get());
        }
        Targets.clear();
    }

private:
    OutputScenario Scenario;
    OutputResult& Result;
    uint32_t Updates{0};
    vector<RenderOutputId> Ids;
    vector<render::test::RenderTarget> Targets;
};

class RenderOutputTest : public testing::TestWithParam<render::RenderBackend> {
protected:
    void SetUp() override {
        render::test::DeviceContext probe;
        if (!render::test::TryCreateDevice(GetParam(), probe, true)) {
            if (render::test::SetupMustFail(probe.Status, render::test::RequiredBackend(GetParam()))) FAIL() << probe.Reason;
            GTEST_SKIP() << probe.Reason;
        }
    }
};
TEST_P(RenderOutputTest, ExplicitZeroOffscreenMultipleSkipAndFailureWorkloads) {
    for (const auto scenario : {OutputScenario::Zero, OutputScenario::External, OutputScenario::Multiple, OutputScenario::Skip, OutputScenario::Failure, OutputScenario::FinalRead}) {
        OutputResult result;
        test::RuntimeLogCapture logs;
        OutputApp app(scenario, result);
        ASSERT_EQ(app.Run({.Backend = GetParam(), .EnableValidation = true, .Multithreaded = true, .WindowTitle = "Render output integration", .WindowWidth = 96, .WindowHeight = 64, .FlightDataCount = 2, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}), 0);
        EXPECT_GT(result.Rendered, 0u);
        if (scenario == OutputScenario::Zero) EXPECT_EQ(result.SideEffects, result.Rendered);
        EXPECT_EQ(result.Available, result.Rendered * (scenario == OutputScenario::Zero ? 0 : scenario == OutputScenario::Multiple ? 2
                                                                                                                                   : 1));
        if (scenario == OutputScenario::Failure) {
            EXPECT_TRUE(result.FailureObserved);
            EXPECT_NE(logs.Errors().find("UninitializedRead"), string::npos);
            EXPECT_EQ(logs.Errors().find("Validation Error"), string::npos);
            EXPECT_EQ(logs.Errors().find("D3D12 ERROR"), string::npos);
        } else
            EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    }
}
INSTANTIATE_TEST_SUITE_P(Backends, RenderOutputTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
