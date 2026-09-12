#include "foundation_graph_fixture.h"
#include "graph_compile_device.h"
#include "failing_graph_command.h"

namespace radray {
namespace {
enum class FailurePoint { None,
                          Texture,
                          View,
                          Framebuffer,
                          Upload,
                          Sampler,
                          SetCreate,
                          SetWrite,
                          SetFlush };
class FailingSet final : public render::ShaderParameterSet {
public:
    FailingSet(unique_ptr<render::ShaderParameterSet> set, FailurePoint point, uint32_t nth,
               uint32_t& calls, uint32_t& flushed, uint32_t& alive)
        : Native(std::move(set)), Point(point), Nth(nth), Calls(calls), Flushed(flushed), Alive(alive) { ++Alive; }
    ~FailingSet() noexcept override { --Alive; }
    bool IsValid() const noexcept override { return Native->IsValid(); }
    void Destroy() noexcept override { Native->Destroy(); }
    bool Set(render::BindingHandle binding, uint32_t element, render::ShaderParameterValue value) noexcept override {
        return !(Point == FailurePoint::SetWrite && ++Calls == Nth) && Native->Set(binding, element, value);
    }
    bool FlushWrites() noexcept override {
        if (Point == FailurePoint::SetFlush && ++Calls == Nth) return false;
        if (!Native->FlushWrites()) return false;
        ++Flushed;
        return true;
    }

private:
    unique_ptr<render::ShaderParameterSet> Native;
    FailurePoint Point;
    uint32_t Nth;
    uint32_t& Calls;
    uint32_t& Flushed;
    uint32_t& Alive;
};
class FaultDevice final : public test::GraphCompileDevice {
public:
    explicit FaultDevice(render::Device& device) : Native(device) {}
    void Arm(FailurePoint point, uint32_t nth) {
        Point = point;
        Nth = nth;
        Calls = 0;
        SuccessfulFlushes = 0;
    }
    bool Fail(FailurePoint point) { return Point == point && ++Calls == Nth; }
    uint32_t GetFailureCalls() const noexcept { return Calls; }
    render::RenderBackend GetBackend() noexcept override { return Native.GetBackend(); }
    render::DeviceDetail GetDetail() const noexcept override { return Native.GetDetail(); }
    const render::RenderDeviceCapabilities& GetCapabilities() const noexcept override { return Native.GetCapabilities(); }
    render::TextureSupport QueryTextureSupport(const render::TextureSupportQuery& query) const noexcept override { return Native.QueryTextureSupport(query); }
    Nullable<unique_ptr<render::Shader>> CreateShader(const render::ShaderDescriptor& desc) noexcept override { return Native.CreateShader(desc); }
    Nullable<unique_ptr<render::GraphicsPipelineState>> CreateGraphicsPipelineState(const render::GraphicsPipelineStateDescriptor& desc) noexcept override { return Native.CreateGraphicsPipelineState(desc); }
    Nullable<unique_ptr<render::ComputePipelineState>> CreateComputePipelineState(const render::ComputePipelineStateDescriptor& desc) noexcept override { return Native.CreateComputePipelineState(desc); }
    Nullable<unique_ptr<render::Texture>> CreateTexture(const render::TextureDescriptor& desc) noexcept override { return Fail(FailurePoint::Texture) ? nullptr : Native.CreateTexture(desc); }
    Nullable<unique_ptr<render::TextureView>> CreateTextureView(const render::TextureViewDescriptor& desc) noexcept override { return Fail(FailurePoint::View) ? nullptr : Native.CreateTextureView(desc); }
    Nullable<unique_ptr<render::Framebuffer>> CreateFramebuffer(const render::FramebufferDescriptor& desc) noexcept override { return Fail(FailurePoint::Framebuffer) ? nullptr : Native.CreateFramebuffer(desc); }
    Nullable<unique_ptr<render::RenderPass>> CreateRenderPass(const render::RenderPassDescriptor& desc) noexcept override { return Native.CreateRenderPass(desc); }
    Nullable<unique_ptr<render::Buffer>> CreateBuffer(const render::BufferDescriptor& desc) noexcept override { return desc.Usage.HasFlag(render::BufferUse::CBuffer) && Fail(FailurePoint::Upload) ? nullptr : Native.CreateBuffer(desc); }
    Nullable<render::Sampler*> GetOrCreateSampler(const render::SamplerDescriptor& desc) noexcept override { return Fail(FailurePoint::Sampler) ? nullptr : Native.GetOrCreateSampler(desc); }
    void FlushMappedRanges(std::span<const render::MappedBufferRange> ranges) noexcept override { Native.FlushMappedRanges(ranges); }
    Nullable<unique_ptr<render::ShaderParameterSet>> CreateShaderParameterSet(const render::ShaderParameterSetDescriptor& desc) noexcept override {
        if (Fail(FailurePoint::SetCreate)) return nullptr;
        auto native = Native.CreateShaderParameterSet(desc);
        if (!native || (Point != FailurePoint::SetWrite && Point != FailurePoint::SetFlush)) return native;
        unique_ptr<render::ShaderParameterSet> wrapper = make_unique<FailingSet>(native.Release(), Point, Nth, Calls, SuccessfulFlushes, LiveFailedSets);
        return wrapper;
    }
    uint32_t LiveFailedSets{0};
    uint32_t SuccessfulFlushes{0};

private:
    render::Device& Native;
    FailurePoint Point{FailurePoint::None};
    uint32_t Nth{1}, Calls{0};
};
class GraphPreparationTest : public test::FoundationGraphGpuTest {};

constexpr std::string_view kBoundaryShader = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) RWStructuredBuffer<uint> Output : register(u0);
[shader("compute")][numthreads(1, 1, 1)] void CSMain() { Output[0] = 73; }
)hlsl";
enum class BoundaryFailure { None,
                             Missing,
                             Duplicate,
                             Geometry,
                             ReadyCallback };
struct BoundaryProgress {
    uint32_t Prepared{0}, Validated{0}, Recorded{0};
};
struct BoundaryProbe {
    BoundaryProgress* Progress;
    RenderGraphFrameResources* Resources;
    bool Reject;
};
struct BoundaryPass {
    BoundaryProgress* Progress;
    RenderGraphFrameResources* Resources;
    RenderGraph* Graph;
    ShaderProgram* Program;
    BoundaryFailure Failure;
    RgBufferValue Buffer;
    Nullable<render::Buffer*> Native{nullptr};
    Nullable<render::ComputePipelineState*> Pipeline{nullptr};
    PreparedShaderGroup Set;
};
RgReadbackTicket AddBoundaryPasses(RenderGraph& graph, ShaderProgram& program, RenderGraphFrameResources& resources,
                                   BoundaryProgress& progress, BoundaryFailure failure) {
    graph.SetCompileOptions({.ReuseResources = false});
    RgBufferValue last;
    for (uint32_t index = 0; index < 3; ++index) {
        last = graph.CreateBuffer({4, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::Vertex | render::BufferUse::CopySource, {}}, "boundary output");
        graph.AddComputePass<BoundaryPass>("boundary parameters", [&](BoundaryPass& data, RenderGraphComputeBuilder& builder) {
                data.Progress = &progress; data.Resources = &resources; data.Graph = &graph; data.Program = &program;
                data.Failure = index == 0 ? failure : BoundaryFailure::None;
                data.Buffer = builder.WriteBuffer(last, RgBufferAccess::UnorderedAccess);
                builder.SetSideEffect(); }, +[](BoundaryPass& data, RenderGraphPrepareContext& context) {
                ++data.Progress->Prepared;
                string declaration{"Output"};
                vector<RgParameterBinding> bindings{{declaration, 0, RgBufferParameterBinding{data.Buffer, {0, 4}, 4}}};
                if (data.Failure == BoundaryFailure::Missing) bindings.clear();
                if (data.Failure == BoundaryFailure::Duplicate) bindings.push_back(bindings.front());
                data.Set = context.CreateParameterSet(*data.Program, 0, bindings);
                declaration.assign(100, 'x');
                data.Native = context.GetBuffer(data.Buffer);
                data.Pipeline = context.ResolveComputePipeline(*data.Program);
                if (data.Failure == BoundaryFailure::Geometry)
                    EXPECT_TRUE(context.ValidateGeometryBuffer(data.Native, RgBufferAccess::Vertex));
                if (context.IsValidationFull()) {
                    EXPECT_EQ(data.Resources->GetParameterSetCount(), 0u);
                    EXPECT_EQ(data.Graph->GetReport().ValidateReadyFrameCalls, 0u);
                    EXPECT_EQ(data.Graph->GetReport().GeometryValidationCalls, 0u);
                    EXPECT_TRUE(data.Graph->GetReport().Diagnostics.empty());
                    context.DeferReadyValidation(make_shared<BoundaryProbe>(BoundaryProbe{data.Progress, data.Resources, data.Failure == BoundaryFailure::ReadyCallback}),
                        +[](const void* pointer, RenderGraphPrepareContext& ready) {
                            const auto& probe = *static_cast<const BoundaryProbe*>(pointer);
                            EXPECT_EQ(probe.Progress->Prepared, 3u);
                            EXPECT_EQ(probe.Resources->GetParameterSetCount(), 0u);
                            ++probe.Progress->Validated;
                            if (probe.Reject) ready.Reject("BoundaryProbe", "Prepared data was rejected at the shared boundary");
                            return !probe.Reject;
                        });
                }
                return data.Set.IsValid() && bool(data.Pipeline); }, +[](const BoundaryPass& data, RenderGraphComputeContext& context) {
                ++data.Progress->Recorded;
                EXPECT_EQ(context.GetBuffer(data.Buffer), data.Native.Get());
                context.Encoder().BindComputePipelineState(data.Pipeline.Get());
                context.Encoder().BindShaderParameterSet(data.Set);
                context.Encoder().Dispatch(1, 1, 1); });
    }
    return graph.ReadbackBuffer("boundary readback", last);
}

TEST_P(GraphPreparationTest, FullSemanticChecksWaitForAllPreparationAndDiscardDrafts) {
    auto& native = *Context.Device;
    FaultDevice fault{native};
    auto program = test::CompileFoundationCompute(native, kBoundaryShader, {}, &fault);
    ASSERT_TRUE(program);
    render::RenderPassRegistry registry{&fault};
    RenderGraphFrameResources resources{fault, registry};
    HostWriteBatch writes;
    uint64_t serial = 0;
    const array cases{std::pair{BoundaryFailure::Missing, "MissingParameterBinding"}, std::pair{BoundaryFailure::Duplicate, "DuplicateParameterBinding"},
                      std::pair{BoundaryFailure::Geometry, "UndeclaredGeometryRead"}, std::pair{BoundaryFailure::ReadyCallback, "BoundaryProbe"}};
    for (const auto& [failure, code] : cases) {
        SCOPED_TRACE(code);
        resources.BeginFlight(++serial, writes);
        // Any attempted write would fail first. A semantic failure must reject all drafts before
        // native descriptor writes start, and must release the wrappers before Execute returns.
        fault.Arm(FailurePoint::SetWrite, 1);
        RenderGraph graph{fault, resources, registry, "deferred semantic failure"};
        BoundaryProgress progress;
        AddBoundaryPasses(graph, *program, resources, progress, failure);
        auto command = native.CreateCommandBuffer(Context.Queue);
        ASSERT_TRUE(command);
        command->Begin();
        test::FailingGraphCommand recording{*command};
        const auto result = RenderGraphTestDriver::Execute(graph, recording, command.Get());
        command->End();
        EXPECT_FALSE(result.Success);
        EXPECT_FALSE(result.CommandsRecorded);
        EXPECT_EQ(recording.RecordingCalls, 0u);
        EXPECT_EQ(progress.Prepared, 3u);
        EXPECT_EQ(progress.Recorded, 0u);
        EXPECT_EQ(graph.GetReport().ValidateReadyFrameCalls, 1u);
        EXPECT_EQ(graph.GetReport().FirstErrorCode, code) << graph.GetReport().ToText();
        EXPECT_EQ(resources.GetParameterSetCount(), 0u);
        EXPECT_EQ(fault.GetFailureCalls(), 0u);
        EXPECT_EQ(fault.LiveFailedSets, 0u);
        EXPECT_EQ(fault.SuccessfulFlushes, 0u);
    }
    resources.Clear();
}

TEST_P(GraphPreparationTest, FailedNativeWriteBatchPublishesNoPartialCacheAndNextFrameRecovers) {
    auto& native = *Context.Device;
    FaultDevice fault{native};
    auto program = test::CompileFoundationCompute(native, kBoundaryShader, {}, &fault);
    ASSERT_TRUE(program);
    render::RenderPassRegistry registry{&fault};
    RenderGraphFrameResources resources{fault, registry};
    HostWriteBatch writes;
    uint64_t serial = 0;
    for (const auto point : {FailurePoint::SetWrite, FailurePoint::SetFlush}) {
        for (uint32_t attempt = 0; attempt < 2; ++attempt) {
            SCOPED_TRACE(fmt::format("point {} attempt {}", uint32_t(point), attempt));
            resources.BeginFlight(++serial, writes);
            fault.Arm(attempt ? FailurePoint::None : point, 2);
            RenderGraph graph{fault, resources, registry, "atomic parameter batch"};
            BoundaryProgress progress;
            auto readback = AddBoundaryPasses(graph, *program, resources, progress, BoundaryFailure::None);
            auto command = native.CreateCommandBuffer(Context.Queue);
            ASSERT_TRUE(command);
            command->Begin();
            test::FailingGraphCommand recording{*command};
            const auto result = RenderGraphTestDriver::Execute(graph, attempt ? *command.Get() : recording, command.Get());
            writes.Flush(native);
            command->End();
            EXPECT_EQ(progress.Prepared, 3u);
            EXPECT_EQ(progress.Validated, 3u);
            if (!attempt) {
                EXPECT_FALSE(result.Success);
                EXPECT_FALSE(result.CommandsRecorded);
                EXPECT_EQ(recording.RecordingCalls, 0u);
                EXPECT_EQ(progress.Recorded, 0u);
                EXPECT_EQ(graph.GetReport().FirstErrorCode, point == FailurePoint::SetWrite ? "ParameterSetWrite" : "ParameterSetFlush");
                EXPECT_EQ(fault.SuccessfulFlushes, 1u);
                EXPECT_EQ(fault.LiveFailedSets, 0u);
                EXPECT_EQ(resources.GetParameterSetCount(), 0u);
            } else {
                ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
                EXPECT_EQ(progress.Recorded, 3u);
                EXPECT_EQ(resources.GetParameterSetCount(), 3u);
                auto* raw = command.Get();
                Context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
                RenderGraphTestDriver::Submitted(raw);
                Context.Queue->Wait();
                RenderGraphTestDriver::Completed(raw);
                vector<byte> bytes;
                ASSERT_TRUE(readback.Read(bytes));
                ASSERT_EQ(bytes.size(), 4u);
                uint32_t value;
                std::memcpy(&value, bytes.data(), 4);
                EXPECT_EQ(value, 73u);
            }
        }
    }
    resources.Clear();
}

TEST_P(GraphPreparationTest, OffFullOffKeepsNativeResolutionAndGpuOutputIdentical) {
    auto& device = *Context.Device;
    auto program = test::CompileFoundationCompute(device, kBoundaryShader);
    ASSERT_TRUE(program);
    uint64_t serial = 0;
    for (const auto mode : {RenderValidationMode::Off, RenderValidationMode::Full, RenderValidationMode::Off}) {
        Resources->BeginFlight(++serial, Writes);
        RenderGraph graph{device, *Resources, *Registry, "validation transition", {mode, RenderGraphReportMode::Full, false}};
        BoundaryProgress progress;
        auto readback = AddBoundaryPasses(graph, *program, *Resources, progress, BoundaryFailure::None);
        ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
        EXPECT_EQ(progress.Prepared, 3u);
        EXPECT_EQ(progress.Recorded, 3u);
        EXPECT_EQ(progress.Validated, mode == RenderValidationMode::Full ? 3u : 0u);
        EXPECT_EQ(graph.GetReport().ValidatePlanInputCalls, mode == RenderValidationMode::Full ? 1u : 0u);
        EXPECT_EQ(graph.GetReport().ValidateReadyFrameCalls, mode == RenderValidationMode::Full ? 1u : 0u);
        EXPECT_EQ(Resources->GetParameterSetCount(), 3u);
        vector<byte> bytes;
        ASSERT_TRUE(readback.Read(bytes));
        ASSERT_EQ(bytes.size(), 4u);
        uint32_t value;
        std::memcpy(&value, bytes.data(), 4);
        EXPECT_EQ(value, 73u);
    }
    Resources->Clear();
}

TEST_P(GraphPreparationTest, B07L07AllocationAndParameterFailuresRecordNoCommandsAndRecover) {
    auto& device = *Context.Device;
    FaultDevice fault{device};
    auto program = test::CompileFoundationGraphics(device, R"hlsl(
#include <core/platform.hlsli>
struct Data { float4 Value; };
VK_BINDING(0, 0) ConstantBuffer<Data> Values : register(b0);
VK_BINDING(1, 0) Texture2D<float> A : register(t0);
VK_BINDING(2, 0) Texture2D<float> B : register(t1);
VK_BINDING(3, 0) SamplerState PointSampler : register(s0);
[shader("vertex")] float4 VSMain(uint id : SV_VertexID) : SV_Position { return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1); }
[shader("pixel")] float PSMain() : SV_Target0 { return A.SampleLevel(PointSampler, float2(.5, .5), 0) + B.SampleLevel(PointSampler, float2(.5, .5), 0) + Values.Value.x; }
)hlsl",
                                                   {}, &fault);
    ASSERT_TRUE(program);
    const auto pitch = Align(uint64_t{16 * 4}, device.GetDetail().TextureDataPitchAlignment);
    for (const auto point : {FailurePoint::Texture, FailurePoint::View, FailurePoint::Framebuffer, FailurePoint::Upload, FailurePoint::Sampler, FailurePoint::SetCreate, FailurePoint::SetWrite, FailurePoint::SetFlush}) {
        for (uint32_t nth = 1; nth <= (point == FailurePoint::Texture || point == FailurePoint::View || point == FailurePoint::Framebuffer || point == FailurePoint::SetCreate ? 2u : 1u); ++nth) {
            SCOPED_TRACE(fmt::format("failure {} allocation {}", uint32_t(point), nth));
            render::RenderPassRegistry registry{&fault};
            RenderGraphFrameResources resources{fault, registry};
            HostWriteBatch writes;
            auto readback = device.CreateBuffer({pitch * 16, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
            ASSERT_TRUE(readback);
            RenderExternalBuffer external{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
            for (uint32_t attempt = 0; attempt < 2; ++attempt) {
                writes.Reset();
                resources.BeginFlight(attempt + 1, writes);
                fault.Arm(attempt ? FailurePoint::None : point, nth);
                RenderGraph graph{fault, resources, registry, "prepare failure recovery"};
                array<RgTextureValue, 4> images;
                for (uint32_t i = 0; i < 4; ++i) images[i] = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource, {}}, fmt::format("image {}", i));
                for (uint32_t i = 0; i < 2; ++i) graph.AddRasterPass<test::EmptyGraphPass>("clear", [=](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, images[i], {.Clear = {.25f * (i + 1), 0, 0, 0}}); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
                struct Draw {
                    ShaderProgram* Program;
                    PreparedShaderGroup Set;
                    Nullable<render::GraphicsPipelineState*> Pipeline;
                    render::RenderBackend Backend;
                    array<RgTextureViewHandle, 3> Textures;
                    array<float, 4> Value;
                };
                for (uint32_t i = 2; i < 4; ++i) graph.AddRasterPass<Draw>("parameters", [&](Draw& data, RenderGraphRasterBuilder& builder) {
                        data.Program = program.Get(); data.Backend = GetParam(); builder.SetColorAttachment(0, images[i]);
                        data.Value = {float(i - 1), 0, 0, 0};
                        data.Textures[0] = builder.ReadTexture(images[i == 2 ? 0 : 2]);
                        data.Textures[1] = builder.ReadTexture(images[1]);
                        data.Textures[2] = builder.ReadTexture(images[0]); }, +[](Draw& data, RenderGraphPrepareContext& ctx) {
                        const RgParameterBinding bindings[]{
                            {"Values", 0, RgCBufferParameterBinding{std::as_bytes(std::span{data.Value})}},
                            {"A", 0, RgTextureParameterBinding{data.Textures[0]}},
                            {"B", 0, RgTextureParameterBinding{data.Textures[1]}},
                            {"PointSampler", 0, RgSamplerParameterBinding{}}};
                        data.Set = ctx.CreateParameterSet(*data.Program, 0, bindings);
                        MaterialPipelineState state; state.Primitive.Cull = render::CullMode::None;
                        state.DepthStencil.DepthTestEnable = state.DepthStencil.DepthWriteEnable = false;
                        data.Pipeline = ctx.ResolveGraphicsPipeline(*data.Program, state);
                        return data.Set.IsValid() && bool(data.Pipeline); }, +[](const Draw& data, RenderGraphRasterContext& context) {
                        context.Encoder().BindGraphicsPipelineState(data.Pipeline.Get());
                        context.Encoder().BindShaderParameterSet(data.Set);
                        context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 16, 16));
                        context.Encoder().SetScissor({0, 0, 16, 16});
                        context.Encoder().Draw(3, 1, 0, 0); });
                const auto host = graph.NextVersion(graph.ImportBuffer(external, "readback", RenderGraphExternalAccess::ObservableOutput));
                graph.AddCopyTextureToBufferPass("copy", images[3], host);
                HostRead(graph, host);
                auto command = device.CreateCommandBuffer(Context.Queue);
                ASSERT_TRUE(command);
                command->Begin();
                test::FailingGraphCommand recordingProbe{*command};
                const auto result = RenderGraphTestDriver::Execute(graph, attempt ? *command.Get() : recordingProbe, command.Get());
                if (!attempt) {
                    EXPECT_FALSE(result.Success);
                    EXPECT_FALSE(result.CommandsRecorded);
                    EXPECT_EQ(recordingProbe.RecordingCalls, 0u);
                    ASSERT_FALSE(graph.GetReport().Diagnostics.empty());
                    if (point != FailurePoint::SetWrite && point != FailurePoint::SetFlush) EXPECT_GE(fault.GetFailureCalls(), nth);
                    EXPECT_EQ(fault.LiveFailedSets, 0u);
                } else {
                    ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
                }
                writes.Flush(device);
                command->End();
                if (result.CommandsRecorded) {
                    auto* raw = command.Get();
                    Context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
                    RenderGraphTestDriver::Submitted(raw);
                    Context.Queue->Wait();
                    RenderGraphTestDriver::Completed(raw);
                }
                if (attempt) {
                    const auto bytes = Read(*readback);
                    float value;
                    std::memcpy(&value, bytes.data(), 4);
                    EXPECT_FLOAT_EQ(value, 4.25f);
                }
            }
            resources.Clear();
            EXPECT_EQ(resources.GetPoolStats().TextureCount, 0u);
            EXPECT_EQ(registry.GetFramebufferCount(), 0u);
            EXPECT_EQ(fault.LiveFailedSets, 0u);
        }
    }
}

// The prepare stage is gated on liveness, so a culled pass must never create descriptors or
// resolve pipeline states for work the graph already decided not to record.
TEST_P(GraphPreparationTest, CulledPassIsNotPrepared) {
    auto& device = *Context.Device;
    render::RenderPassRegistry registry{&device};
    RenderGraphFrameResources resources{device, registry};
    HostWriteBatch writes;
    resources.BeginFlight(1, writes);
    RenderGraph graph{device, resources, registry, "culled prepare"};
    const auto target = [&](std::string_view name) {
        return graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, name);
    };
    struct Counted {
        uint32_t* Prepares;
    };
    uint32_t live = 0, culled = 0;
    graph.AddRasterPass<Counted>("live", [&](Counted& data, RenderGraphRasterBuilder& builder) {
            data.Prepares = &live;
            builder.SetColorAttachment(0, target("observed"), {.Clear = {1, 0, 0, 0}});
            builder.SetSideEffect(); }, +[](Counted& data, RenderGraphPrepareContext&) { ++*data.Prepares; return true; }, +[](const Counted&, RenderGraphRasterContext&) {});
    graph.AddRasterPass<Counted>("culled", [&](Counted& data, RenderGraphRasterBuilder& builder) {
            data.Prepares = &culled;
            builder.SetColorAttachment(0, target("dropped"), {.Clear = {0, 1, 0, 0}}); }, +[](Counted& data, RenderGraphPrepareContext&) { ++*data.Prepares; return true; }, +[](const Counted&, RenderGraphRasterContext&) {});
    auto command = device.CreateCommandBuffer(Context.Queue);
    ASSERT_TRUE(command);
    command->Begin();
    const auto result = RenderGraphTestDriver::Execute(graph, *command.Get(), command.Get());
    writes.Flush(device);
    command->End();
    ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
    EXPECT_EQ(graph.GetReport().LivePasses, 1u);
    EXPECT_EQ(live, 1u);
    EXPECT_EQ(culled, 0u);
    if (result.CommandsRecorded) {
        auto* raw = command.Get();
        Context.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
        RenderGraphTestDriver::Submitted(raw);
        Context.Queue->Wait();
        RenderGraphTestDriver::Completed(raw);
    }
    resources.Clear();
}
INSTANTIATE_TEST_SUITE_P(Backends, GraphPreparationTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
