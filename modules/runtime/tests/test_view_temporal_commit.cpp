#include "runtime_test_support.h"
#include "foundation_graph_fixture.h"
#include "failing_graph_command.h"
#include <radray/runtime/render_system.h>
#include <radray/runtime/render_framework/renderer_list_pass_bindings.h>

namespace radray {
namespace {

class ViewTemporalGpuTest : public test::FoundationGraphGpuTest {};

HistoryTextureRequest HistoryRequest(std::string_view name, uint32_t count = 2, HistoryCommitMode mode = HistoryCommitMode::WithView) {
    HistoryTextureRequest request;
    request.Key = name;
    request.DebugName = name;
    request.BufferCount = count;
    request.CommitMode = mode;
    request.Desc.Format = render::TextureFormat::RGBA8_UNORM;
    request.Desc.Usage = render::TextureUse::RenderTarget | render::TextureUse::Resource;
    request.Desc.Extent.Mode = RenderExtentMode::RelativeToFamilyRenderExtent;
    return request;
}

RgPassHandle WriteHistory(RenderGraph& graph, HistoryTexturePair& pair, std::string_view name) {
    const auto texture = graph.ImportTexture(*pair.Current, name, RenderGraphExternalAccess::ObservableOutput);
    return graph.AddRasterPass<test::EmptyGraphPass>(name, [=](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, texture, {.Clear = {{.2f, .4f, .6f, 1}}}); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
}

TEST_P(ViewTemporalGpuTest, T03T11AtomicHistoryGroupsRotationAndTokenRejection) {
    ViewStateRegistry registry(*Context.Device, *Registry, 3);
    ResolvedRenderViewFamily family;
    family.OutputAvailable = true;
    family.RenderSize = family.OutputSize = {16, 16};
    family.OutputFormat = render::TextureFormat::RGBA8_UNORM;
    uint64_t serial = 0;
    for (uint32_t count : {2u, 3u, 4u}) {
        ResolvedRenderView view;
        view.StateId = AllocateViewStateId();
        view.ViewProjection.setIdentity();
        HistoryWriteToken old;
        uint32_t commits = 0;
        for (uint32_t frame = 0; frame < count * 2 + 2; ++frame) {
            ++serial;
            registry.BeginFlight(static_cast<uint32_t>(serial % 3), serial);
            if (serial > 1) Resources->BeginFlight(serial, Writes);
            registry.Resolve(view, family);
            string reason;
            auto color = registry.AcquireHistoryTexture(view, family, HistoryRequest("color", count), reason);
            auto depth = registry.AcquireHistoryTexture(view, family, HistoryRequest("depth", count), reason);
            ASSERT_TRUE(color.Current) << reason;
            ASSERT_TRUE(depth.Current) << reason;
            EXPECT_EQ(color.CommitToken.Index, commits % count);
            EXPECT_FALSE(registry.CommitHistory(color.CommitToken));
            EXPECT_FALSE(registry.AcquireHistoryTexture(view, family, HistoryRequest("color", count), reason).Current);
            auto graph = MakeGraph("atomic history");
            WriteHistory(graph, color, "color");
            if (frame != 1) WriteHistory(graph, depth, "depth");
            ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
            const array tokens{color.CommitToken, depth.CommitToken};
            auto invalid = tokens;
            invalid[0].Index = count;
            EXPECT_FALSE(registry.CommitViewWithHistory(view.StateId, invalid));
            invalid = tokens;
            invalid[1] = invalid[0];
            EXPECT_FALSE(registry.CommitViewWithHistory(view.StateId, invalid));
            if (old.Generation) EXPECT_FALSE(registry.CommitViewWithHistory(view.StateId, std::span{&old, 1}));
            const auto previousSerial = registry.GetCommittedSerial(view.StateId);
            if (frame == 1) {
                EXPECT_FALSE(registry.CommitViewWithHistory(view.StateId, tokens));
                EXPECT_EQ(registry.GetCommittedSerial(view.StateId), previousSerial);
            } else {
                EXPECT_TRUE(registry.CommitViewWithHistory(view.StateId, tokens));
                EXPECT_FALSE(registry.CommitViewWithHistory(view.StateId, tokens));
                EXPECT_EQ(registry.GetCommittedSerial(view.StateId), serial);
                ++commits;
            }
            old = color.CommitToken;
        }
    }
    ResolvedRenderView view;
    view.StateId = AllocateViewStateId();
    view.ViewProjection.setIdentity();
    registry.BeginFlight(0, ++serial);
    registry.Resolve(view, family);
    string reason;
    for (const uint32_t count : {1u, 5u}) EXPECT_FALSE(registry.AcquireHistoryTexture(view, family, HistoryRequest("invalid", count), reason).Current);
}

struct TemporalHostResult {
    uint32_t Frames{0};
    uint32_t PsoFailures{0};
    array<float, 2> LastX{0, 0};
    array<uint64_t, 2> Commits{0, 0};
};

TEST_P(ViewTemporalGpuTest, T04LateRasterAndComputeEncoderFailurePreservesTheWholeHistoryGroup) {
    ViewStateRegistry registry(*Context.Device, *Registry, 3);
    ResolvedRenderViewFamily family;
    family.OutputAvailable = true;
    family.OutputSize = family.RenderSize = {16, 16};
    family.OutputFormat = render::TextureFormat::RGBA8_UNORM;
    uint64_t serial = 0;
    for (const bool compute : {false, true}) {
        ResolvedRenderView view;
        view.StateId = AllocateViewStateId();
        view.ViewProjection.setIdentity();
        array<render::Texture*, 2> committed{};
        for (uint32_t frame = 0; frame < 3; ++frame) {
            ++serial;
            registry.BeginFlight(static_cast<uint32_t>((serial - 1) % 3), serial);
            Resources->BeginFlight(serial, Writes);
            registry.Resolve(view, family);
            string reason;
            auto color = registry.AcquireHistoryTexture(view, family, HistoryRequest("color"), reason);
            auto depthRequest = HistoryRequest("depth");
            depthRequest.Desc.Usage |= render::TextureUse::UnorderedAccess;
            auto depth = registry.AcquireHistoryTexture(view, family, depthRequest, reason);
            ASSERT_TRUE(color.Current);
            ASSERT_TRUE(depth.Current);
            if (frame > 0) {
                EXPECT_EQ(color.Previous->Texture, committed[0]);
                EXPECT_EQ(depth.Previous->Texture, committed[1]);
            }
            auto graph = MakeGraph("late history failure");
            WriteHistory(graph, color, "color written before failure");
            if (frame == 1 && compute) {
                const auto texture = graph.ImportTexture(*depth.Current, "depth", RenderGraphExternalAccess::ObservableOutput);
                graph.AddComputePass<test::EmptyGraphPass>("fail compute", [=](test::EmptyGraphPass&, RenderGraphComputeBuilder& builder) { builder.WriteTexture(texture); }, +[](const test::EmptyGraphPass&, RenderGraphComputeContext&) {});
            } else
                WriteHistory(graph, depth, "depth");
            auto command = Context.Device->CreateCommandBuffer(Context.Queue);
            ASSERT_TRUE(command);
            command->Begin();
            test::FailingGraphCommand failure(*command);
            failure.PassesBeforeFailure = 1;
            const auto result = RenderGraphTestDriver::Execute(graph, frame == 1 ? failure : *command.Get());
            EXPECT_EQ(result.Success, frame != 1);
            EXPECT_TRUE(result.CommandsRecorded);
            EXPECT_TRUE(graph.GetReport().Passes[0].Executed);
            EXPECT_EQ(graph.GetReport().Passes[1].Executed, frame != 1);
            Writes.Flush(*Context.Device);
            command->End();
            auto* raw = command.Get();
            Context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
            Context.Queue->Wait();
            const array tokens{color.CommitToken, depth.CommitToken};
            EXPECT_EQ(registry.CommitViewWithHistory(view.StateId, tokens), frame != 1);
            if (frame == 0) committed = {color.Current->Texture, depth.Current->Texture};
        }
    }
}

class TemporalProbePipeline : public RenderPipeline {
public:
    explicit TemporalProbePipeline(TemporalHostResult& result) : Result(result) {}
#if defined(RADRAY_ENABLE_SHADER_JIT)
    void InitializeGraphics(render::Device& device) {
        auto program = test::CompileFoundationGraphics(device, R"hlsl(
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position { return float4(p, 1); }
[shader("pixel")] float4 PSMain() : SV_Target0 { return float4(.25, .5, .75, 1); }
)hlsl");
        ASSERT_TRUE(program);
        Program = program.Release();
        const array<float, 9> positions{-1, -1, .5f, 3, -1, .5f, -1, 3, .5f};
        const array<uint32_t, 3> indices{0, 1, 2};
        auto vertex = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{positions}), render::BufferUse::Vertex);
        auto index = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
        ASSERT_TRUE(vertex);
        ASSERT_TRUE(index);
        Vertex = vertex.Release();
        Index = index.Release();
        Geometry.VertexBuffers = {{ 0,
                                    { Vertex.get(),
                                      0,
                                      sizeof(positions) } }};
        Geometry.Ibv = {Index.get(), 0, 4};
        Geometry.VertexLayout.Buffers = {{ 0,
                                           12,
                                           render::VertexStepMode::Vertex }};
        Geometry.VertexLayout.Attributes = {{ "POSITION",
                                              0,
                                              0,
                                              0,
                                              render::VertexFormat::FLOAT32X3 }};
        for (const bool writeDepth : {false, true}) {
            MeshDrawCommand draw;
            draw.Program = Program.get();
            draw.Geometry = &Geometry;
            draw.IndexCount = 3;
            draw.PipelineState.Primitive.Cull = render::CullMode::None;
            draw.PipelineState.DepthStencil.DepthTestEnable = true;
            draw.PipelineState.DepthStencil.DepthWriteEnable = writeDepth;
            ASSERT_TRUE(FinalizeMeshDrawCommand(draw));
            RequiredList.Commands.push_back(std::move(draw));
        }
        Backend = device.GetBackend();
    }
#endif
    void PrepareFrame(RenderPrepareContext& context) override {
        ++Frame;
        for (const auto& output : context.Outputs) {
            if (!output.Active || output.Kind != RenderOutputKind::Presentation) continue;
            RenderViewFamilyDesc family;
            family.Output = output.Id;
            for (uint32_t i = 0; i < 2; ++i) {
                RenderViewDesc view;
                view.StateId = Ids[i];
                view.Name = i == 0 ? "A" : "B";
                view.WorldToView(0, 3) = static_cast<float>(Frame);
                view.ViewRect = {(Frame >= 7 ? 1 - i : i) * .5f, 0, .5f, 1};
                view.ScissorRect = view.ViewRect;
                family.Views.push_back(view);
            }
            context.Workloads.AddViewFamily(std::move(family));
        }
    }
    void Render(RenderPipelineContext& context) override {
        RequiredDraws = {};
        ASSERT_EQ(context.ViewFamilies().size(), 1u);
        const auto& family = context.ViewFamilies().front();
        ASSERT_TRUE(family.OutputAvailable);
        auto graph = context.CreateRenderGraph("temporal context probe");
        const auto output = context.ImportOutput(graph, family.OutputId);
        RenderSceneSnapshot snapshot;
        snapshot.Primitives.emplace_back();
        snapshot.Primitives[0].Generation = 42;
        snapshot.Primitives[0].LocalToWorld(0, 3) = static_cast<float>(Frame);
        array<ViewCompletionToken, 2> tokens;
        for (uint32_t i = 0; i < 2; ++i) {
            auto view = family.Views[i];
            if (Frame == 7) context.InvalidateView(view.StateId);
            ASSERT_TRUE(context.PreparePrimitiveHistory(view, snapshot));
            const auto motion = context.GetPrimitiveMotion(view.StateId, snapshot.Primitives[0]);
            EXPECT_EQ(motion.Valid, Result.Commits[i] > 0 && Frame != 7);
            if (motion.Valid) EXPECT_FLOAT_EQ(motion.PreviousLocalToWorld(0, 3), Result.LastX[i]);
            string reason;
            auto color = context.AcquireHistoryTexture(view, family, HistoryRequest("color"), reason);
            auto depth = context.AcquireHistoryTexture(view, family, HistoryRequest("depth"), reason);
            ASSERT_TRUE(color.Current) << reason;
            ASSERT_TRUE(depth.Current) << reason;
            EXPECT_EQ(color.PreviousValid, Result.Commits[i] > 0 && Frame != 7);
            WriteHistory(graph, color, fmt::format("color {}", i));
            if (!(Frame == 4 && i == 0)) WriteHistory(graph, depth, fmt::format("depth {}", i));
            const bool outputWritten = Frame != 2 && !(Frame == 3 && i == 1);
            RgPassHandle completion;
            if (outputWritten) {
#if defined(RADRAY_ENABLE_SHADER_JIT)
                if (Frame == 5 && i == 0) {
                    const auto depthTarget = graph.CreateTexture({render::TextureDimension::Dim2D, family.OutputSize.Width, family.OutputSize.Height, 1, 1, 1, render::TextureFormat::D32_FLOAT, render::MemoryType::Device, render::TextureUse::DepthStencilRead | render::TextureUse::DepthStencilWrite, {}}, "PSO rejection depth");
                    graph.AddRasterPass<test::EmptyGraphPass>("initialize depth", [=](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { builder.SetDepthAttachment(depthTarget); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&){});
                    struct Draw {
                        RendererList* List;
                        DrawExecutionStats* Stats;
                        render::RenderBackend Backend;
                    };
                    completion = graph.AddRasterPass<Draw>("required draw PSO failure", [&](Draw& data, RenderGraphRasterBuilder& builder) {
                        data = {&RequiredList, &RequiredDraws, Backend};
                        builder.SetColorAttachment(0, output);
                        builder.SetDepthAttachment(depthTarget, {.Load = render::LoadAction::Load, .ReadOnly = true}); }, +[](const Draw& data, RenderGraphRasterContext& pass) {
                        pass.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 96, 64));
                        pass.Encoder().SetScissor({0, 0, 96, 64});
                        SubmitRendererList(*data.List, pass, pass.PassState(), *data.Stats); });
                } else
#endif
                    completion = graph.AddRasterPass<test::EmptyGraphPass>(fmt::format("complete {}", i), [=](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, output, {.Load = i == 0 ? render::LoadAction::Clear : render::LoadAction::Load}); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
            } else {
                completion = graph.AddComputePass<test::EmptyGraphPass>(fmt::format("not complete {}", i), [](test::EmptyGraphPass&, RenderGraphComputeBuilder&) {}, +[](const test::EmptyGraphPass&, RenderGraphComputeContext&) {});
            }
            tokens[i] = context.RegisterViewCompletion(graph, view.StateId, completion);
            ASSERT_TRUE(tokens[i].IsValid());
            EXPECT_FALSE(context.RegisterViewCompletion(graph, view.StateId, completion).IsValid());
        }
        auto independent = context.AcquireHistoryTexture(family.Views[0], family, HistoryRequest("feedback", 2, HistoryCommitMode::Independent), Reason);
        ASSERT_TRUE(independent.Current);
        EXPECT_EQ(independent.PreviousValid, Frame > 1);
        WriteHistory(graph, independent, "independent feedback");
        ASSERT_TRUE(context.ExecuteGraph(graph).Success);
#if defined(RADRAY_ENABLE_SHADER_JIT)
        if (Frame == 5) {
            EXPECT_EQ(RequiredDraws.Draws, 1u);
            EXPECT_EQ(RequiredDraws.PsoFailure, 1u);
            Result.PsoFailures += static_cast<uint32_t>(RequiredDraws.PsoFailure);
        }
#endif
        EXPECT_FALSE(context.CommitView(Ids[1], tokens[0], true));
        if (Old.IsValid()) EXPECT_FALSE(context.CommitView(Ids[0], Old, true));
        for (uint32_t i = 0; i < 2; ++i) {
            const bool drawsSucceeded = i != 0 || Frame != 5 ||
#if defined(RADRAY_ENABLE_SHADER_JIT)
                                        RequiredDraws.Succeeded();
#else
                                        false;
#endif
            const bool expected = Frame != 2 && !(Frame == 3 && i == 1) && !(Frame == 4 && i == 0) && drawsSucceeded;
            const bool committed = context.CommitView(Ids[i], tokens[i], drawsSucceeded);
            EXPECT_EQ(committed, expected) << "frame " << Frame << " view " << i;
            EXPECT_FALSE(context.CommitView(Ids[i], tokens[i], true));
            if (committed) {
                Result.LastX[i] = static_cast<float>(Frame);
                ++Result.Commits[i];
            }
        }
        Old = tokens[0];
        ++Result.Frames;
    }

private:
    TemporalHostResult& Result;
    uint32_t Frame{0};
    array<ViewStateId, 2> Ids{AllocateViewStateId(), AllocateViewStateId()};
    ViewCompletionToken Old;
    string Reason;
    DrawExecutionStats RequiredDraws;
#if defined(RADRAY_ENABLE_SHADER_JIT)
    unique_ptr<ShaderProgram> Program;
    unique_ptr<render::Buffer> Vertex, Index;
    GpuMesh::DrawData Geometry;
    RendererList RequiredList;
    render::RenderBackend Backend{};
#endif
};

class TemporalHost : public Application {
public:
    explicit TemporalHost(TemporalHostResult& result) : Result(result) {}
    void OnInit() override {
        auto pipeline = make_unique<TemporalProbePipeline>(Result);
#if defined(RADRAY_ENABLE_SHADER_JIT)
        pipeline->InitializeGraphics(*GetDevice());
#endif
        GetRenderSystem()->SetPipeline(std::move(pipeline));
    }
    void OnUpdate(const AppUpdateContext&) override {
        if (Result.Frames >= 8 || ++Updates >= 12) test::CloseMainWindow(*this);
    }

private:
    TemporalHostResult& Result;
    uint32_t Updates{0};
};

class ViewTemporalContextTest : public testing::TestWithParam<render::RenderBackend> {};
TEST_P(ViewTemporalContextTest, T01T02T03T04T08T12OutputProofDrawFailureAndIndependentHistory) {
    {
        render::test::DeviceContext device;
        if (!render::test::TryCreateDevice(GetParam(), device, true)) GTEST_SKIP() << device.Reason;
    }
    TemporalHostResult result;
    test::RuntimeLogCapture logs;
    TemporalHost app(result);
    ASSERT_EQ(app.Run({.Backend = GetParam(), .EnableValidation = true, .Multithreaded = false, .WindowTitle = "Temporal contract test", .WindowWidth = 96, .WindowHeight = 64, .FlightDataCount = 3, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}), 0);
    EXPECT_GE(result.Frames, 8u);
#if defined(RADRAY_ENABLE_SHADER_JIT)
    EXPECT_EQ(result.PsoFailures, 1u);
    RecordProperty("actual_required_pso_failures", result.PsoFailures);
    EXPECT_EQ(logs.Errors(), "RendererList PSO failure in pass 3 for program 0 batch 0\n");
#else
    RecordProperty("actual_required_pso_failures", "not_built_without_jit");
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
#endif
}

INSTANTIATE_TEST_SUITE_P(Backends, ViewTemporalGpuTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
INSTANTIATE_TEST_SUITE_P(Backends, ViewTemporalContextTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
