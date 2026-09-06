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
    FailingSet(unique_ptr<render::ShaderParameterSet> set, FailurePoint point, uint32_t& alive) : Native(std::move(set)), Point(point), Alive(alive) { ++Alive; }
    ~FailingSet() noexcept override { --Alive; }
    bool IsValid() const noexcept override { return Native->IsValid(); }
    void Destroy() noexcept override { Native->Destroy(); }
    bool Set(render::BindingHandle binding, uint32_t element, render::ShaderParameterValue value) noexcept override {
        return Point != FailurePoint::SetWrite && Native->Set(binding, element, value);
    }
    bool FlushWrites() noexcept override { return Point != FailurePoint::SetFlush && Native->FlushWrites(); }

private:
    unique_ptr<render::ShaderParameterSet> Native;
    FailurePoint Point;
    uint32_t& Alive;
};
class FaultDevice final : public test::GraphCompileDevice {
public:
    explicit FaultDevice(render::Device& device) : Native(device) {}
    void Arm(FailurePoint point, uint32_t nth) {
        Point = point;
        Nth = nth;
        Calls = 0;
    }
    bool Fail(FailurePoint point) { return Point == point && ++Calls == Nth; }
    uint32_t GetFailureCalls() const noexcept { return Calls; }
    render::RenderBackend GetBackend() noexcept override { return Native.GetBackend(); }
    render::DeviceDetail GetDetail() const noexcept override { return Native.GetDetail(); }
    const render::RenderDeviceCapabilities& GetCapabilities() const noexcept override { return Native.GetCapabilities(); }
    render::TextureSupport QueryTextureSupport(const render::TextureSupportQuery& query) const noexcept override { return Native.QueryTextureSupport(query); }
    Nullable<unique_ptr<render::Shader>> CreateShader(const render::ShaderDescriptor& desc) noexcept override { return Native.CreateShader(desc); }
    Nullable<unique_ptr<render::GraphicsPipelineState>> CreateGraphicsPipelineState(const render::GraphicsPipelineStateDescriptor& desc) noexcept override { return Native.CreateGraphicsPipelineState(desc); }
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
        unique_ptr<render::ShaderParameterSet> wrapper = make_unique<FailingSet>(native.Release(), Point, LiveFailedSets);
        return wrapper;
    }
    uint32_t LiveFailedSets{0};

private:
    render::Device& Native;
    FailurePoint Point{FailurePoint::None};
    uint32_t Nth{1}, Calls{0};
};
class GraphPreparationTest : public test::FoundationGraphGpuTest {};

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
                array<RgTextureHandle, 4> images;
                for (uint32_t i = 0; i < 4; ++i) images[i] = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource, {}}, fmt::format("image {}", i));
                for (uint32_t i = 0; i < 2; ++i) graph.AddRasterPass<test::EmptyGraphPass>("clear", [=](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, images[i], {.Clear = {.25f * (i + 1), 0, 0, 0}}); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
                struct Draw {
                    ShaderProgram* Program;
                    RgParameterSetHandle Set;
                    render::RenderBackend Backend;
                };
                for (uint32_t i = 2; i < 4; ++i) graph.AddRasterPass<Draw>("parameters", [&](Draw& data, RenderGraphRasterBuilder& builder) {
                    data.Program = program.Get(); data.Backend = GetParam(); builder.SetColorAttachment(0, images[i]);
                    const array<float, 4> value{float(i - 1), 0, 0, 0};
                    const RgParameterBinding bindings[]{{"Values", 0, RgCBufferParameterBinding{std::as_bytes(std::span{value})}},
                        {"A", 0, RgTextureParameterBinding{images[i == 2 ? 0 : 2]}}, {"B", 0, RgTextureParameterBinding{images[1]}}, {"PointSampler", 0, RgSamplerParameterBinding{}}};
                    data.Set = builder.CreateParameterSet(*program, 0, bindings); }, +[](const Draw& data, RenderGraphRasterContext& context) {
                    MaterialPipelineState state; state.Primitive.Cull = render::CullMode::None; state.DepthStencil.DepthTestEnable = state.DepthStencil.DepthWriteEnable = false;
                    const auto pso = data.Program->GetOrCreateGraphicsPipelineState(state, {}, PrimitiveTopology::TriangleList, context.PassState()); ASSERT_TRUE(pso);
                    context.Encoder().BindGraphicsPipelineState(pso.Get()); context.BindParameterSet(data.Set);
                    context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 16, 16)); context.Encoder().SetScissor({0, 0, 16, 16}); context.Encoder().Draw(3, 1, 0, 0); });
                const auto host = graph.ImportBuffer(external, "readback", RenderGraphExternalAccess::ObservableOutput);
                graph.AddCopyTextureToBufferPass("copy", images[3], host);
                HostRead(graph, host);
                auto command = device.CreateCommandBuffer(Context.Queue);
                ASSERT_TRUE(command);
                command->Begin();
                test::FailingGraphCommand recordingProbe{*command};
                const auto result = RenderGraphTestDriver::Execute(graph, attempt ? *command.Get() : recordingProbe);
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
                    Context.Queue->Wait();
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
INSTANTIATE_TEST_SUITE_P(Backends, GraphPreparationTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
