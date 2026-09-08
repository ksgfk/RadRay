#include "gpu_test_fixture.h"
#include "render_graph_test_driver.h"
#include "foundation_shader_fixture.h"
#include "failing_graph_command.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <radray/utility.h>
#include <radray/runtime/render_framework/render_graph_runtime.h>
#include <radray/runtime/render_framework/viewport.h>

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
class RenderGraphTest : public testing::TestWithParam<render::RenderBackend> {
protected:
    void SetUp() override {
        if (!render::test::TryCreateDevice(GetParam(), DeviceContext, true)) GTEST_SKIP() << DeviceContext.Reason;
        Registry = make_unique<render::RenderPassRegistry>(DeviceContext.Device.get());
        FrameResources = make_unique<RenderGraphFrameResources>(*DeviceContext.Device, *Registry);
        BeginFlight(1);
    }
    void TearDown() override {
        if (DeviceContext.Queue) DeviceContext.Queue->Wait();
        FrameResources.reset();
        Registry.reset();
        DeviceContext.Reset();
        EXPECT_EQ(DeviceContext.ValidationErrors.load(), 0u);
    }
    void BeginFlight(uint64_t serial) {
        Writes.Reset();
        FrameResources->BeginFlight(serial, Writes);
    }
    RenderGraph MakeGraph(std::string_view name = "test") { return RenderGraph{*DeviceContext.Device, *FrameResources, *Registry, name}; }
    void Submit(render::CommandBuffer& command) {
        Writes.Flush(*DeviceContext.Device);
        command.End();
        auto* raw = &command;
        DeviceContext.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
        RenderGraphTestDriver::Submitted(raw);
        DeviceContext.Queue->Wait();
        RenderGraphTestDriver::Completed(raw);
    }
    render::test::DeviceContext DeviceContext;
    HostWriteBatch Writes;
    unique_ptr<render::RenderPassRegistry> Registry;
    unique_ptr<RenderGraphFrameResources> FrameResources;
};

TEST_P(RenderGraphTest, CopyBufferRoundTripAndNoCommandsOnCompileFailure) {
    auto& device = *DeviceContext.Device;
    const array<uint32_t, 4> expected{7, 19, 1337, 0xffffffff};
    auto upload = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{expected}), render::BufferUse::CopySource);
    auto readback = device.CreateBuffer({16, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
    ASSERT_TRUE(upload);
    ASSERT_TRUE(readback);
    RenderExternalBuffer source{upload.Get(), upload->GetDesc(), render::BufferState::HostWrite, true};
    RenderExternalBuffer output{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    auto graph = MakeGraph();
    auto src = graph.ImportBuffer(source, "upload", RenderGraphExternalAccess::ReadOnly);
    auto dst = graph.NextVersion(graph.ImportBuffer(output, "readback", RenderGraphExternalAccess::ObservableOutput));
    auto intermediate = graph.CreateBuffer({16, render::MemoryType::Device, render::BufferUse::CopySource | render::BufferUse::CopyDestination, {}}, "intermediate");
    graph.AddCopyBufferPass("upload", src, intermediate, 16);
    graph.AddCopyBufferPass("readback", intermediate, dst, 16);
    graph.AddComputePass<EmptyPass>("host visibility", [=](EmptyPass&, RenderGraphComputeBuilder& builder) {
        builder.ReadBuffer(dst, RgBufferAccess::HostRead); builder.SetSideEffect(); }, EmptyCompute);
    auto command = device.CreateCommandBuffer(DeviceContext.Queue);
    ASSERT_TRUE(command);
    command->Begin();
    auto result = RenderGraphTestDriver::Execute(graph, *command);
    ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
    Submit(*command);
    auto* mapped = readback->Map(0, 16);
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange({0, 16});
    EXPECT_EQ(std::memcmp(mapped, expected.data(), 16), 0);
    readback->Unmap();
    EXPECT_TRUE(output.ContentValid);
    EXPECT_EQ(output.State, render::BufferState::HostRead);
    EXPECT_FALSE(RenderGraphTestDriver::Execute(graph, *command).Success);
    auto bad = MakeGraph();
    auto tex = bad.CreateTexture(GraphColor(), "undefined");
    Clear(bad, tex, "bad load", render::LoadAction::Load, render::StoreAction::Store, true);
    auto failed = RenderGraphTestDriver::Execute(bad, *command);
    EXPECT_FALSE(failed.Success);
    EXPECT_FALSE(failed.CommandsRecorded);
}

TEST_P(RenderGraphTest, MipClearReadbackAndPhysicalStateSurvivesFlightReuse) {
    auto& device = *DeviceContext.Device;
    const uint64_t row = Align(uint64_t{8 * 4}, device.GetDetail().TextureDataPitchAlignment);
    auto readback = device.CreateBuffer({row * 8, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(readback);
    RenderExternalBuffer output{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    for (uint64_t frame = 1; frame <= 102; ++frame) {
        if (frame > 1) BeginFlight(frame);
        auto graph = MakeGraph();
        auto color = graph.CreateTexture(GraphColor(2), "mipped");
        auto dst = graph.NextVersion(graph.ImportBuffer(output, "readback", RenderGraphExternalAccess::ObservableOutput));
        Clear(graph, color, "mip one", render::LoadAction::Clear, render::StoreAction::Store, false, 1);
        graph.AddCopyTextureToBufferPass("readback", color, dst, {0, 1, 1, 1});
        graph.AddComputePass<EmptyPass>("host", [=](EmptyPass&, RenderGraphComputeBuilder& builder) { builder.ReadBuffer(dst, RgBufferAccess::HostRead); builder.SetSideEffect(); }, EmptyCompute);
        auto command = device.CreateCommandBuffer(DeviceContext.Queue);
        ASSERT_TRUE(command);
        command->Begin();
        ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, *command).Success) << graph.GetReport().ToText();
        Submit(*command);
        const auto& barriers = graph.GetReport().Barriers;
        EXPECT_EQ(barriers[0].Subresource, 1u);
        EXPECT_EQ(barriers[0].Before, uint32_t(frame == 1 ? render::TextureState::Undefined : render::TextureState::CopySource));
        auto* mapped = static_cast<const uint8_t*>(readback->Map(0, row * 8));
        ASSERT_NE(mapped, nullptr);
        readback->InvalidateMappedRange({0, row * 8});
        EXPECT_NEAR(mapped[0], 64, 1);
        EXPECT_NEAR(mapped[1], 128, 1);
        EXPECT_NEAR(mapped[2], 191, 1);
        EXPECT_EQ(mapped[3], 255);
        readback->Unmap();
    }
    EXPECT_EQ(FrameResources->GetPool().GetStats().Created, 1u);
    EXPECT_EQ(FrameResources->GetPool().GetStats().ViewsCreated, 1u);
    EXPECT_EQ(FrameResources->GetPool().GetStats().Hits, 101u);
    EXPECT_EQ(Registry->GetFramebufferCount(), 1u);
}

TEST_P(RenderGraphTest, EncoderFailureCommitsActualStateBeforeRecovery) {
    auto& device = *DeviceContext.Device;
    for (const bool compute : {false, true}) {
        auto desc = GraphColor();
        desc.Usage |= render::TextureUse::UnorderedAccess;
        auto target = device.CreateTexture(desc);
        ASSERT_TRUE(target);
        array<render::TextureStates, 1> states{render::TextureState::Undefined};
        array<uint8_t, 1> valid{0};
        RenderExternalTexture external{target.Get(), desc, states, valid};
        auto graph = MakeGraph("failure");
        auto output = graph.NextVersion(graph.ImportTexture(external, "output", RenderGraphExternalAccess::ObservableOutput));
        if (compute)
            graph.AddComputePass<EmptyPass>("fail compute", [=](EmptyPass&, RenderGraphComputeBuilder& builder) { builder.WriteTexture(output); }, EmptyCompute);
        else
            Clear(graph, output, "fail raster");
        auto command = device.CreateCommandBuffer(DeviceContext.Queue);
        ASSERT_TRUE(command);
        command->Begin();
        test::FailingGraphCommand failing(*command);
        const auto result = RenderGraphTestDriver::Execute(graph, failing, command.Get());
        EXPECT_FALSE(result.Success);
        EXPECT_TRUE(result.CommandsRecorded);
        EXPECT_FALSE(external.Written);
        EXPECT_EQ(valid[0], 0);
        EXPECT_EQ(states[0], render::TextureState::Undefined);
        Submit(*command);
        command->Begin();
        EXPECT_EQ(states[0], compute ? render::TextureState::UnorderedAccess : render::TextureState::RenderTarget);
        EXPECT_FALSE(graph.GetReport().Passes[0].Executed);
        EXPECT_EQ(graph.GetReport().Diagnostics[0].Code, compute ? "BeginComputePass" : "BeginRenderPass");

        const uint64_t row = Align(uint64_t{16 * 4}, device.GetDetail().TextureDataPitchAlignment);
        auto readback = device.CreateBuffer({row * 16, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
        ASSERT_TRUE(readback);
        RenderExternalBuffer bytes{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
        auto recovery = MakeGraph("recovery");
        auto color = recovery.NextVersion(recovery.ImportTexture(external, "output", RenderGraphExternalAccess::ReadWrite));
        const auto dest = recovery.NextVersion(recovery.ImportBuffer(bytes, "readback", RenderGraphExternalAccess::ObservableOutput));
        Clear(recovery, color);
        recovery.AddCopyTextureToBufferPass("readback", color, dest);
        recovery.AddComputePass<EmptyPass>("host", [=](EmptyPass&, RenderGraphComputeBuilder& builder) { builder.ReadBuffer(dest, RgBufferAccess::HostRead); builder.SetSideEffect(); }, EmptyCompute);
        ASSERT_TRUE(RenderGraphTestDriver::Execute(recovery, *command).Success) << recovery.GetReport().ToText();
        Submit(*command);
        auto* mapped = static_cast<const uint8_t*>(readback->Map(0, row * 16));
        ASSERT_NE(mapped, nullptr);
        readback->InvalidateMappedRange({0, row * 16});
        EXPECT_NEAR(mapped[0], 64, 1);
        EXPECT_NEAR(mapped[1], 128, 1);
        EXPECT_NEAR(mapped[2], 191, 1);
        readback->Unmap();
        BeginFlight(compute ? 3 : 2);
    }
}

TEST_P(RenderGraphTest, ConsecutiveUavWritesUseMemoryBarrier) {
    auto& device = *DeviceContext.Device;
    constexpr std::string_view source = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) RWStructuredBuffer<uint> Value : register(u0);
[shader("compute")][numthreads(1, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) { Value[0] = Value[0] * 3 + 7; }
)hlsl";
    auto program = test::CompileFoundationCompute(device, source);
    ASSERT_TRUE(program);
    const uint32_t initial = 5;
    auto upload = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{&initial, 1}), render::BufferUse::CopySource);
    auto readback = device.CreateBuffer({4, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(upload);
    ASSERT_TRUE(readback);
    RenderExternalBuffer src{upload.Get(), upload->GetDesc(), render::BufferState::HostWrite, true};
    RenderExternalBuffer dst{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    auto graph = MakeGraph();
    auto input = graph.ImportBuffer(src, "upload", RenderGraphExternalAccess::ReadOnly);
    auto output = graph.NextVersion(graph.ImportBuffer(dst, "output", RenderGraphExternalAccess::ObservableOutput));
    auto buffer = graph.CreateBuffer({4, render::MemoryType::Device, render::BufferUse::CopyDestination | render::BufferUse::CopySource | render::BufferUse::UnorderedAccess, {}}, "uav");
    graph.AddCopyBufferPass("initialize", input, buffer, 4);
    struct Data {
        RgComputeProgramHandle Program;
        RgParameterSetHandle Parameters;
    };
    for (uint32_t p = 0; p < 2; ++p) graph.AddComputePass<Data>("transform", [&](Data& data, RenderGraphComputeBuilder& builder) {
        buffer = graph.NextVersion(buffer);
        const array<RgParameterBinding, 1> bindings{{
            {.Declaration = "Value",
             .Value = RgBufferParameterBinding{
                 .Buffer = buffer,
                 .Range = {0, 4},
                 .StructureByteStride = 4,
                 .Access = RgParameterAccess::ReadWrite}}}};
        data = {builder.UseComputeProgram(*program.Get()),
                builder.CreateParameterSet(*program.Get(), 0, bindings)}; }, +[](const Data& data, RenderGraphComputeContext& context) {
        context.BindComputeProgram(data.Program);
        context.BindParameterSet(data.Parameters);
        context.Encoder().Dispatch(1, 1, 1); });
    graph.AddCopyBufferPass("readback", buffer, output, 4);
    graph.AddComputePass<EmptyPass>("host", [=](EmptyPass&, RenderGraphComputeBuilder& builder) { builder.ReadBuffer(output, RgBufferAccess::HostRead); builder.SetSideEffect(); }, EmptyCompute);
    auto command = device.CreateCommandBuffer(DeviceContext.Queue);
    ASSERT_TRUE(command);
    command->Begin();
    ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, *command).Success) << graph.GetReport().ToText();
    Submit(*command);
    EXPECT_EQ(graph.GetReport().UavBarriers, 1u);
    auto* mapped = static_cast<const uint32_t*>(readback->Map(0, 4));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange({0, 4});
    EXPECT_EQ(*mapped, 73u);
    readback->Unmap();
}

TEST_P(RenderGraphTest, FirstReadOnlyUavAccessSynchronizesPreviousGraphSubmission) {
    auto& device = *DeviceContext.Device;
    constexpr std::string_view writeSource = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) RWStructuredBuffer<uint> Value : register(u0);
[shader("compute")][numthreads(1, 1, 1)]
void CSMain() { Value[0] = 73; }
)hlsl";
    constexpr std::string_view readSource = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) RWStructuredBuffer<uint> Value : register(u0);
VK_BINDING(1, 0) RWStructuredBuffer<uint> Output : register(u1);
[shader("compute")][numthreads(1, 1, 1)]
void CSMain() { Output[0] = Value[0] + 9; }
)hlsl";
    auto writer = test::CompileFoundationCompute(device, writeSource);
    auto reader = test::CompileFoundationCompute(device, readSource);
    ASSERT_TRUE(writer);
    ASSERT_TRUE(reader);
    auto buffer = device.CreateBuffer({4, render::MemoryType::Device, render::BufferUse::UnorderedAccess, {}});
    auto output = device.CreateBuffer({4, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::CopySource, {}});
    auto readback = device.CreateBuffer({4, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(buffer);
    ASSERT_TRUE(output);
    ASSERT_TRUE(readback);
    RenderExternalBuffer shared{buffer.Get(), buffer->GetDesc(), render::BufferState::Undefined};
    RenderExternalBuffer result{output.Get(), output->GetDesc(), render::BufferState::Undefined};
    RenderExternalBuffer bytes{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    struct Data {
        RgComputeProgramHandle Program;
        RgParameterSetHandle Parameters;
    };
    const auto dispatch = +[](const Data& data, RenderGraphComputeContext& ctx) {
        ctx.BindComputeProgram(data.Program);
        ctx.BindParameterSet(data.Parameters);
        ctx.Encoder().Dispatch(1, 1, 1);
    };
    auto first = MakeGraph("producer");
    const auto destination = first.NextVersion(first.ImportBuffer(shared, "shared", RenderGraphExternalAccess::ObservableOutput));
    first.AddComputePass<Data>("write", [&](Data& data, RenderGraphComputeBuilder& builder) {
        const array<RgParameterBinding, 1> bindings{{
            {.Declaration = "Value",
             .Value = RgBufferParameterBinding{
                 .Buffer = destination,
                 .Range = {0, 4},
                 .StructureByteStride = 4,
                 .Access = RgParameterAccess::Write}}}};
        data = {builder.UseComputeProgram(*writer.Get()),
                builder.CreateParameterSet(*writer.Get(), 0, bindings)}; }, dispatch);
    auto firstCommand = device.CreateCommandBuffer(DeviceContext.Queue);
    auto secondCommand = device.CreateCommandBuffer(DeviceContext.Queue);
    ASSERT_TRUE(firstCommand);
    ASSERT_TRUE(secondCommand);
    firstCommand->Begin();
    ASSERT_TRUE(RenderGraphTestDriver::Execute(first, *firstCommand).Success);
    firstCommand->End();
    Writes.Flush(device);
    auto* firstNative = firstCommand.Get();
    DeviceContext.Queue->Submit({.CmdBuffers = std::span{&firstNative, 1}});
    RenderGraphTestDriver::Submitted(firstNative);
    ASSERT_EQ(shared.State, render::BufferState::UnorderedAccess);

    auto second = MakeGraph("consumer");
    const auto source = second.ImportBuffer(shared, "shared", RenderGraphExternalAccess::ReadOnly);
    const auto target = second.NextVersion(second.ImportBuffer(result, "result", RenderGraphExternalAccess::ReadWrite));
    const auto host = second.NextVersion(second.ImportBuffer(bytes, "readback", RenderGraphExternalAccess::ObservableOutput));
    second.AddComputePass<Data>("read UAV", [&](Data& data, RenderGraphComputeBuilder& builder) {
        const array<RgParameterBinding, 2> bindings{{
            {.Declaration = "Value",
             .Value = RgBufferParameterBinding{
                 .Buffer = source,
                 .Range = {0, 4},
                 .StructureByteStride = 4,
                 .Access = RgParameterAccess::Read}},
            {.Declaration = "Output",
             .Value = RgBufferParameterBinding{
                 .Buffer = target,
                 .Range = {0, 4},
                 .StructureByteStride = 4,
                 .Access = RgParameterAccess::Write}}}};
        data = {builder.UseComputeProgram(*reader.Get()),
                builder.CreateParameterSet(*reader.Get(), 0, bindings)}; }, dispatch);
    second.AddCopyBufferPass("readback", target, host, 4);
    second.AddComputePass<EmptyPass>("host", [=](EmptyPass&, RenderGraphComputeBuilder& builder) {
        builder.ReadBuffer(host, RgBufferAccess::HostRead); builder.SetSideEffect(); }, EmptyCompute);
    secondCommand->Begin();
    ASSERT_TRUE(RenderGraphTestDriver::Execute(second, *secondCommand).Success);
    EXPECT_EQ(second.GetReport().UavBarriers, 1u);
    Writes.Flush(device);
    Submit(*secondCommand);
    RenderGraphTestDriver::Completed(firstNative);
    auto* mapped = static_cast<const uint32_t*>(readback->Map(0, 4));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange({0, 4});
    EXPECT_EQ(*mapped, 82u);
    readback->Unmap();
}

TEST_P(RenderGraphTest, RasterAndComputeWritesAreSampledAndClearLoadSharePso) {
    auto& device = *DeviceContext.Device;
    constexpr std::string_view graphicsSource = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) Texture2D<float4> InputTexture : register(t0);
[shader("vertex")]
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2 - 1, 0, 1);
}
[shader("pixel")]
float4 PSMain(float4 position : SV_Position) : SV_Target0 { return InputTexture.Load(int3(int2(position.xy), 0)); }
)hlsl";
    constexpr std::string_view computeSource = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) RWTexture2D<float4> OutputTexture : register(u0);
[shader("compute")][numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) { OutputTexture[tid.xy] = float4(.25, .5, .75, 1); }
)hlsl";
    auto graphics = test::CompileFoundationGraphics(device, graphicsSource);
    ASSERT_TRUE(graphics);
    auto compute = test::CompileFoundationCompute(device, computeSource);
    ASSERT_TRUE(compute);
    const uint64_t row = Align(uint64_t{16 * 4}, device.GetDetail().TextureDataPitchAlignment);
    auto readback = device.CreateBuffer({row * 16, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(readback);
    RenderExternalBuffer external{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    for (uint32_t computeFirst = 0; computeFirst < 2; ++computeFirst) {
        if (computeFirst) BeginFlight(2);
        auto graph = MakeGraph();
        auto desc = GraphColor();
        desc.Usage |= render::TextureUse::UnorderedAccess;
        auto input = graph.CreateTexture(desc, "input"), output = graph.CreateTexture(GraphColor(), "output");
        if (!computeFirst)
            Clear(graph, input, "raster producer");
        else {
            struct Data {
                RgComputeProgramHandle Program;
                RgParameterSetHandle Parameters;
            };
            graph.AddComputePass<Data>("compute producer", [&](Data& data, RenderGraphComputeBuilder& builder) {
                const array<RgParameterBinding, 1> bindings{{
                    {.Declaration = "OutputTexture",
                     .Value = RgTextureParameterBinding{
                         .Texture = input,
                         .Access = RgParameterAccess::Write}}}};
                data = {builder.UseComputeProgram(*compute.Get()),
                        builder.CreateParameterSet(*compute.Get(), 0, bindings)}; }, +[](const Data& data, RenderGraphComputeContext& ctx) {
                ctx.BindComputeProgram(data.Program);
                ctx.BindParameterSet(data.Parameters);
                ctx.Encoder().Dispatch(2, 2, 1); });
        }
        struct Data {
            RgParameterSetHandle Parameters;
            ShaderProgram* Program;
            render::RenderBackend Backend;
        };
        for (const auto load : {render::LoadAction::Clear, render::LoadAction::Load}) {
            output = graph.NextVersion(output);
            graph.AddRasterPass<Data>("sample", [&](Data& data, RenderGraphRasterBuilder& builder) {
                const array<RgParameterBinding, 1> bindings{{
                    {.Declaration = "InputTexture",
                     .Value = RgTextureParameterBinding{.Texture = input}}}};
                data = {builder.CreateParameterSet(*graphics.Get(), 0, bindings),
                        graphics.Get(), device.GetBackend()};
                builder.SetColorAttachment(0, output, {.Load = load}); }, +[](const Data& data, RenderGraphRasterContext& ctx) {
                MaterialPipelineState material; material.Primitive.Cull = render::CullMode::None;
                auto pso = data.Program->GetOrCreateGraphicsPipelineState(material, {}, PrimitiveTopology::TriangleList, ctx.PassState());
                ASSERT_TRUE(pso);
                ctx.Encoder().BindGraphicsPipelineState(pso.Get());
                ctx.BindParameterSet(data.Parameters); ctx.Encoder().SetViewport(MakeViewport(data.Backend, 16, 16));
                ctx.Encoder().SetScissor({0, 0, 16, 16}); ctx.Encoder().Draw(3, 1, 0, 0); });
        }
        auto destination = graph.NextVersion(graph.ImportBuffer(external, "readback", RenderGraphExternalAccess::ObservableOutput));
        graph.AddCopyTextureToBufferPass("readback", output, destination);
        graph.AddComputePass<EmptyPass>("host", [=](EmptyPass&, RenderGraphComputeBuilder& builder) { builder.ReadBuffer(destination, RgBufferAccess::HostRead); builder.SetSideEffect(); }, EmptyCompute);
        auto command = device.CreateCommandBuffer(DeviceContext.Queue);
        ASSERT_TRUE(command);
        command->Begin();
        ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, *command).Success) << graph.GetReport().ToText();
        Submit(*command);
        auto* mapped = static_cast<const uint8_t*>(readback->Map(0, row * 16));
        ASSERT_NE(mapped, nullptr);
        readback->InvalidateMappedRange({0, row * 16});
        EXPECT_NEAR(mapped[0], 64, 1);
        EXPECT_NEAR(mapped[1], 128, 1);
        EXPECT_NEAR(mapped[2], 191, 1);
        EXPECT_EQ(mapped[3], 255);
        readback->Unmap();
        EXPECT_EQ(graphics->GetGraphicsPipelineStateCount(), 1u);
    }
}

TEST_P(RenderGraphTest, ComputeIndirectMsaaResolveComputeReadbackChain) {
    auto& device = *DeviceContext.Device;
    const auto& features = device.GetCapabilities().Features;
    if (!features.IndirectDraw || !features.IndirectDispatch) {
        GTEST_SKIP() << "Backend does not expose indirect draw and dispatch";
    }
    const render::TextureUses msaaUsage =
        render::TextureUse::RenderTarget | render::TextureUse::CopySource;
    const render::TextureSupport support = device.QueryTextureSupport(
        {render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, msaaUsage});
    if (!support.Supported || !support.SampleCounts.HasFlag(render::SampleCount::X4)) {
        GTEST_SKIP() << "Backend does not support 4x RGBA8 render-target resolve";
    }

    constexpr std::string_view argumentSource = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(4, 2) RWByteAddressBuffer Arguments : register(u4, space2);
[shader("compute")][numthreads(1, 1, 1)]
void BuildArguments() {
    Arguments.Store(0, 3);
    Arguments.Store(4, 1);
    Arguments.Store(8, 0);
    Arguments.Store(12, 0);
    Arguments.Store(16, 3);
    Arguments.Store(20, 1);
    Arguments.Store(24, 0);
    Arguments.Store(28, 0);
    Arguments.Store(32, 0);
    Arguments.Store(36, 1);
    Arguments.Store(40, 1);
    Arguments.Store(44, 1);
}
)hlsl";
    constexpr std::string_view rasterSource = R"hlsl(
#include <core/platform.hlsli>
[shader("vertex")]
float4 IndirectVS(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2 - 1, 0, 1);
}
[shader("pixel")]
float4 IndirectPS() : SV_Target0 { return float4(1, .25, .5, 1); }
)hlsl";
    constexpr std::string_view postSource = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(5, 3) Texture2D<float4> Resolved : register(t5, space3);
VK_BINDING(7, 3) RWStructuredBuffer<uint> Output : register(u7, space3);
[shader("compute")][numthreads(1, 1, 1)]
void ReadResolved() {
    Output[0] = (uint)round(saturate(Resolved.Load(int3(0, 0, 0)).r) * 255.0f);
}
)hlsl";
    auto argumentProgram = test::CompileFoundationCompute(device, argumentSource);
    auto rasterProgram = test::CompileFoundationGraphics(device, rasterSource);
    auto postProgram = test::CompileFoundationCompute(device, postSource);
    ASSERT_TRUE(argumentProgram);
    ASSERT_TRUE(rasterProgram);
    ASSERT_TRUE(postProgram);
    EXPECT_FALSE(rasterProgram->GetOrCreateComputePipelineState());

    const array<uint32_t, 3> indices{0, 1, 2};
    auto indexBuffer = render::test::MakeUploadBuffer(
        device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
    auto readback = device.CreateBuffer({4, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(indexBuffer);
    ASSERT_TRUE(readback);
    RenderExternalBuffer externalIndex{
        indexBuffer.Get(), indexBuffer->GetDesc(), render::BufferState::HostWrite, true};
    RenderExternalBuffer externalReadback{
        readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};

    auto graph = MakeGraph("indirect resolve integration");
    const auto arguments = graph.CreateBuffer(
        {48, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::Indirect, {}},
        "indirect arguments");
    const auto index = graph.ImportBuffer(
        externalIndex, "indices", RenderGraphExternalAccess::ReadOnly);
    auto msaaDesc = GraphColor();
    msaaDesc.SampleCount = 4;
    msaaDesc.Usage = msaaUsage;
    const auto msaa = graph.CreateTexture(msaaDesc, "msaa color");
    auto resolvedDesc = GraphColor();
    resolvedDesc.Usage = render::TextureUse::CopyDestination | render::TextureUse::Resource;
    const auto resolved = graph.CreateTexture(resolvedDesc, "resolved color");
    const auto result = graph.CreateBuffer(
        {4, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::CopySource, {}},
        "post result");
    const auto host = graph.NextVersion(graph.ImportBuffer(
        externalReadback, "readback", RenderGraphExternalAccess::ObservableOutput));

    struct ComputeData {
        RgComputeProgramHandle Program;
        RgParameterSetHandle Parameters;
        RgIndirectArgumentsHandle Arguments;
    };
    graph.AddComputePass<ComputeData>(
        "build indirect arguments",
        [&](ComputeData& data, RenderGraphComputeBuilder& builder) {
            const array<RgParameterBinding, 1> bindings{{{.Declaration = "Arguments",
                                                          .Value = RgBufferParameterBinding{
                                                              .Buffer = arguments,
                                                              .Range = {0, 48},
                                                              .Access = RgParameterAccess::Write}}}};
            data.Program = builder.UseComputeProgram(*argumentProgram.Get());
            data.Parameters = builder.CreateParameterSet(*argumentProgram.Get(), 2, bindings);
        },
        +[](const ComputeData& data, RenderGraphComputeContext& context) {
            context.BindComputeProgram(data.Program);
            context.BindParameterSet(data.Parameters);
            context.Encoder().Dispatch(1, 1, 1);
        });

    struct RasterData {
        ShaderProgram* Program{nullptr};
        RgBufferValue Index;
        RgIndirectArgumentsHandle Draw;
        RgIndirectArgumentsHandle DrawIndexed;
        render::RenderBackend Backend{render::RenderBackend::D3D12};
    };
    graph.AddRasterPass<RasterData>(
        "indirect raster",
        [&](RasterData& data, RenderGraphRasterBuilder& builder) {
            data.Program = rasterProgram.Get();
            data.Index = builder.ReadBuffer(index, RgBufferAccess::Index);
            data.Draw = builder.ReadIndirectArguments(arguments, RgIndirectCommand::Draw, 0, 1);
            data.DrawIndexed = builder.ReadIndirectArguments(
                arguments, RgIndirectCommand::DrawIndexed, 16, 1);
            data.Backend = device.GetBackend();
            builder.SetColorAttachment(0, msaa);
        },
        +[](const RasterData& data, RenderGraphRasterContext& context) {
            MaterialPipelineState material;
            material.Primitive.Cull = render::CullMode::None;
            const auto pso = data.Program->GetOrCreateGraphicsPipelineState(
                material, {}, PrimitiveTopology::TriangleList, context.PassState());
            ASSERT_TRUE(pso);
            context.Encoder().BindGraphicsPipelineState(pso.Get());
            context.Encoder().BindIndexBuffer({context.GetBuffer(data.Index), 0, 4});
            context.Encoder().SetViewport(MakeViewport(data.Backend, 16, 16));
            context.Encoder().SetScissor({0, 0, 16, 16});
            context.Encoder().DrawIndirect(data.Draw);
            context.Encoder().DrawIndexedIndirect(data.DrawIndexed);
        });

    graph.AddResolveTexturePass("resolve", msaa, resolved);
    graph.AddComputePass<ComputeData>(
        "post process",
        [&](ComputeData& data, RenderGraphComputeBuilder& builder) {
            const array<RgParameterBinding, 2> bindings{{{.Declaration = "Resolved",
                                                          .Value = RgTextureParameterBinding{.Texture = resolved}},
                                                         {.Declaration = "Output",
                                                          .Value = RgBufferParameterBinding{
                                                              .Buffer = result,
                                                              .Range = {0, 4},
                                                              .StructureByteStride = 4,
                                                              .Access = RgParameterAccess::Write}}}};
            data.Program = builder.UseComputeProgram(*postProgram.Get());
            data.Parameters = builder.CreateParameterSet(*postProgram.Get(), 3, bindings);
            data.Arguments = builder.ReadIndirectArguments(
                arguments, RgIndirectCommand::Dispatch, 36, 1);
        },
        +[](const ComputeData& data, RenderGraphComputeContext& context) {
            context.BindComputeProgram(data.Program);
            context.BindParameterSet(data.Parameters);
            context.Encoder().DispatchIndirect(data.Arguments);
        });
    graph.AddCopyBufferPass("readback", result, host, 4);
    graph.AddComputePass<EmptyPass>(
        "host visibility",
        [=](EmptyPass&, RenderGraphComputeBuilder& builder) {
            builder.ReadBuffer(host, RgBufferAccess::HostRead);
            builder.SetSideEffect();
        },
        EmptyCompute);

    auto command = device.CreateCommandBuffer(DeviceContext.Queue);
    ASSERT_TRUE(command);
    command->Begin();
    ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, *command).Success)
        << graph.GetReport().ToText();
    Submit(*command);
    auto* mapped = static_cast<const uint32_t*>(readback->Map(0, 4));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange({0, 4});
    EXPECT_EQ(*mapped, 255u);
    readback->Unmap();
    ASSERT_EQ(graph.GetReport().LivePasses, 6u);
    EXPECT_EQ(graph.GetReport().Passes[2].Type, RgPassType::Resolve);
}

TEST_P(RenderGraphTest, ResolveArrayLayersRoundTrip) {
    auto& device = *DeviceContext.Device;
    const render::TextureUses sourceUsage =
        render::TextureUse::RenderTarget | render::TextureUse::CopySource;
    const render::TextureSupport support = device.QueryTextureSupport(
        {render::TextureDimension::Dim2DArray, render::TextureFormat::RGBA8_UNORM, sourceUsage});
    if (!support.Supported || !support.SampleCounts.HasFlag(render::SampleCount::X4) ||
        support.MaxArrayLayers < 2) {
        GTEST_SKIP() << "Backend does not support two-layer 4x RGBA8 resolve sources";
    }
    const uint64_t row = Align(uint64_t{8 * 4}, device.GetDetail().TextureDataPitchAlignment);
    auto readback = device.CreateBuffer({row * 8, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(readback);
    RenderExternalBuffer hostExternal{
        readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    auto graph = MakeGraph("array resolve");
    const auto source = graph.CreateTexture(
        {render::TextureDimension::Dim2DArray, 8, 8, 2, 1, 4, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, sourceUsage, {}},
        "msaa layers");
    const auto resolved = graph.CreateTexture(
        {render::TextureDimension::Dim2DArray, 8, 8, 2, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::CopyDestination | render::TextureUse::CopySource, {}},
        "resolved layers");
    const auto host = graph.NextVersion(graph.ImportBuffer(
        hostExternal, "readback", RenderGraphExternalAccess::ObservableOutput));
    graph.AddRasterPass<EmptyPass>(
        "clear layers",
        [=](EmptyPass&, RenderGraphRasterBuilder& builder) {
            builder.SetColorAttachment(
                0, source,
                {.View = {
                     .Dimension = render::TextureDimension::Dim2DArray,
                     .Range = {0, 2, 0, 1}},
                 .Clear = {.25f, .5f, .75f, 1}});
        },
        EmptyRaster);
    graph.AddResolveTexturePass(
        "resolve layers", source, resolved, {0, 2, 0, 1}, {0, 2, 0, 1});
    graph.AddCopyTextureToBufferPass("copy layer one", resolved, host, {1, 1, 0, 1});
    graph.AddComputePass<EmptyPass>(
        "host visibility",
        [=](EmptyPass&, RenderGraphComputeBuilder& builder) {
            builder.ReadBuffer(host, RgBufferAccess::HostRead);
            builder.SetSideEffect();
        },
        EmptyCompute);
    auto command = device.CreateCommandBuffer(DeviceContext.Queue);
    ASSERT_TRUE(command);
    command->Begin();
    ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, *command).Success)
        << graph.GetReport().ToText();
    Submit(*command);
    auto* mapped = static_cast<const uint8_t*>(readback->Map(0, row * 8));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange({0, row * 8});
    EXPECT_NEAR(mapped[0], 64, 1);
    EXPECT_NEAR(mapped[1], 128, 1);
    EXPECT_NEAR(mapped[2], 191, 1);
    EXPECT_EQ(mapped[3], 255);
    readback->Unmap();
}

TEST_P(RenderGraphTest, ParameterSnapshotsArraysDynamicOffsetsAndImmutableSamplerSurviveGraph) {
    auto& device = *DeviceContext.Device;
    constexpr std::string_view source = R"hlsl(
#include <core/platform.hlsli>
struct Parameters {
    uint Value;
    uint Index;
    uint2 Padding;
};
VK_BINDING(3, 2) ConstantBuffer<Parameters> Values : register(b3, space2);
VK_BINDING(5, 4) RWStructuredBuffer<uint> Outputs[2] : register(u5, space4);
VK_BINDING(1, 5) Texture2D<float4> Image : register(t1, space5);
VK_BINDING(2, 5) SamplerState FixedSampler : register(s2, space5);
VK_BINDING(0, 6) StructuredBuffer<uint> Structured : register(t0, space6);
VK_BINDING(1, 6) ByteAddressBuffer Raw : register(t1, space6);
VK_BINDING(2, 6) Buffer<uint> Typed : register(t2, space6);
#define RS \
    "CBV(b3, space=2)," \
    "DescriptorTable(UAV(u5, numDescriptors=2, space=4))," \
    "DescriptorTable(SRV(t1, space=5))," \
    "StaticSampler(s2, space=5, filter=FILTER_MIN_MAG_MIP_POINT)," \
    "DescriptorTable(SRV(t0, numDescriptors=3, space=6))"
[shader("compute")]
[RootSignature(RS)]
[numthreads(1, 1, 1)]
void ParameterMain() {
    uint sampled = (uint)round(Image.SampleLevel(FixedSampler, float2(.5, .5), 0).r * 100.0f);
    Outputs[Values.Index][0] = Values.Value + Structured[0] + Raw.Load(0) + Typed[0] + sampled;
}
)hlsl";
    render::ShaderProgramLayoutRecipe recipe;
    recipe.Vulkan.BufferDescriptors.push_back({.Selector = {
                                                   .DeclarationName = "Values",
                                                   .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer},
                                               .Placement = render::VulkanBufferDescriptorPlacement::Dynamic});
    auto program = test::CompileFoundationCompute(device, source, recipe);
    ASSERT_TRUE(program);
    const auto samplerInfo = program->GetArtifact().FindBindingInfo("FixedSampler");
    ASSERT_TRUE(samplerInfo.has_value());
    EXPECT_TRUE(samplerInfo->Immutable);
    const auto valuesInfo = program->GetArtifact().FindBindingInfo("Values");
    ASSERT_TRUE(valuesInfo.has_value());
    EXPECT_TRUE(valuesInfo->Dynamic);
    const auto outputsInfo = program->GetArtifact().FindBindingInfo("Outputs");
    ASSERT_TRUE(outputsInfo.has_value());
    EXPECT_EQ(outputsInfo->Count, 2u);

    const uint32_t structuredValue = 3;
    const uint32_t rawValue = 5;
    const uint32_t typedValue = 7;
    auto structuredBuffer = render::test::MakeUploadBuffer(
        device, std::as_bytes(std::span{&structuredValue, 1}), render::BufferUse::Resource);
    auto rawBuffer = render::test::MakeUploadBuffer(
        device, std::as_bytes(std::span{&rawValue, 1}), render::BufferUse::Resource);
    auto typedBuffer = render::test::MakeUploadBuffer(
        device, std::as_bytes(std::span{&typedValue, 1}), render::BufferUse::Resource);
    auto firstReadback = device.CreateBuffer({4, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    auto secondReadback = device.CreateBuffer({4, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(structuredBuffer);
    ASSERT_TRUE(rawBuffer);
    ASSERT_TRUE(typedBuffer);
    ASSERT_TRUE(firstReadback);
    ASSERT_TRUE(secondReadback);
    RenderExternalBuffer structuredExternal{
        structuredBuffer.Get(), structuredBuffer->GetDesc(), render::BufferState::HostWrite, true};
    RenderExternalBuffer rawExternal{
        rawBuffer.Get(), rawBuffer->GetDesc(), render::BufferState::HostWrite, true};
    RenderExternalBuffer typedExternal{
        typedBuffer.Get(), typedBuffer->GetDesc(), render::BufferState::HostWrite, true};
    RenderExternalBuffer firstHost{
        firstReadback.Get(), firstReadback->GetDesc(), render::BufferState::CopyDestination};
    RenderExternalBuffer secondHost{
        secondReadback.Get(), secondReadback->GetDesc(), render::BufferState::CopyDestination};

    struct alignas(16) Constants {
        uint32_t Value;
        uint32_t Index;
        uint32_t Padding[2];
    };
    static_assert(sizeof(Constants) == 16);
    struct Data {
        RgComputeProgramHandle Program;
        array<RgParameterSetHandle, 3> Resources;
        array<RgParameterSetHandle, 2> Constants;
    };
    const auto execute = +[](const Data& data, RenderGraphComputeContext& context) {
        context.BindComputeProgram(data.Program);
        for (const RgParameterSetHandle resources : data.Resources) {
            context.BindParameterSet(resources);
        }
        context.BindParameterSet(data.Constants[0]);
        context.Encoder().Dispatch(1, 1, 1);
        context.BindParameterSet(data.Constants[1]);
        context.Encoder().Dispatch(1, 1, 1);
    };

    for (uint64_t flight = 1; flight <= 2; ++flight) {
        if (flight > 1) BeginFlight(flight);
        const uint32_t firstValue = flight == 1 ? 11u : 101u;
        const uint32_t secondValue = flight == 1 ? 22u : 202u;
        Constants first{firstValue, 0, {0, 0}};
        Constants second{secondValue, 1, {0, 0}};
        auto command = device.CreateCommandBuffer(DeviceContext.Queue);
        ASSERT_TRUE(command);
        command->Begin();
        {
            auto graph = MakeGraph("parameter lifetime");
            const auto structured = graph.ImportBuffer(
                structuredExternal, "structured", RenderGraphExternalAccess::ReadOnly);
            const auto raw = graph.ImportBuffer(
                rawExternal, "raw", RenderGraphExternalAccess::ReadOnly);
            const auto typed = graph.ImportBuffer(
                typedExternal, "typed", RenderGraphExternalAccess::ReadOnly);
            const auto host0 = graph.NextVersion(graph.ImportBuffer(
                firstHost, "first readback", RenderGraphExternalAccess::ObservableOutput));
            const auto host1 = graph.NextVersion(graph.ImportBuffer(
                secondHost, "second readback", RenderGraphExternalAccess::ObservableOutput));
            const auto output0 = graph.CreateBuffer(
                {4, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::CopySource, {}},
                "output zero");
            const auto output1 = graph.CreateBuffer(
                {4, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::CopySource, {}},
                "output one");
            auto imageDesc = GraphColor();
            imageDesc.Usage = render::TextureUse::RenderTarget | render::TextureUse::Resource;
            auto image = graph.CreateTexture(imageDesc, "sampled image");
            Clear(graph, image, "image clear");
            graph.AddComputePass<Data>(
                "parameter dispatches",
                [&](Data& data, RenderGraphComputeBuilder& builder) {
                    const array<RgParameterBinding, 2> outputBindings{{{.Declaration = "Outputs",
                                                                        .ArrayElement = 0,
                                                                        .Value = RgBufferParameterBinding{
                                                                            .Buffer = output0,
                                                                            .Range = {0, 4},
                                                                            .StructureByteStride = 4,
                                                                            .Access = RgParameterAccess::Write}},
                                                                       {.Declaration = "Outputs", .ArrayElement = 1, .Value = RgBufferParameterBinding{.Buffer = output1, .Range = {0, 4}, .StructureByteStride = 4, .Access = RgParameterAccess::Write}}}};
                    const array<RgParameterBinding, 1> imageBindings{{{.Declaration = "Image",
                                                                       .Value = RgTextureParameterBinding{.Texture = image}}}};
                    const array<RgParameterBinding, 3> sourceBindings{{{.Declaration = "Structured",
                                                                        .Value = RgBufferParameterBinding{
                                                                            .Buffer = structured,
                                                                            .Range = {0, 4},
                                                                            .StructureByteStride = 4}},
                                                                       {.Declaration = "Raw", .Value = RgBufferParameterBinding{.Buffer = raw, .Range = {0, 4}}},
                                                                       {.Declaration = "Typed", .Value = RgBufferParameterBinding{.Buffer = typed, .Range = {0, 4}, .Format = render::TextureFormat::R32_UINT}}}};
                    const array<RgParameterBinding, 1> firstBindings{{{.Declaration = "Values",
                                                                       .Value = RgCBufferParameterBinding{
                                                                           .Bytes = std::as_bytes(std::span{&first, 1})}}}};
                    const array<RgParameterBinding, 1> secondBindings{{{.Declaration = "Values",
                                                                        .Value = RgCBufferParameterBinding{
                                                                            .Bytes = std::as_bytes(std::span{&second, 1})}}}};
                    data.Program = builder.UseComputeProgram(*program.Get());
                    data.Resources = {
                        builder.CreateParameterSet(*program.Get(), 4, outputBindings),
                        builder.CreateParameterSet(*program.Get(), 5, imageBindings),
                        builder.CreateParameterSet(*program.Get(), 6, sourceBindings)};
                    data.Constants = {
                        builder.CreateParameterSet(*program.Get(), 2, firstBindings),
                        builder.CreateParameterSet(*program.Get(), 2, secondBindings)};
                    first.Value = 999;
                    second.Value = 999;
                },
                execute);
            graph.AddCopyBufferPass("first copy", output0, host0, 4);
            graph.AddCopyBufferPass("second copy", output1, host1, 4);
            graph.AddComputePass<EmptyPass>(
                "host visibility",
                [=](EmptyPass&, RenderGraphComputeBuilder& builder) {
                    builder.ReadBuffer(host0, RgBufferAccess::HostRead);
                    builder.ReadBuffer(host1, RgBufferAccess::HostRead);
                    builder.SetSideEffect();
                },
                EmptyCompute);
            ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, *command).Success)
                << graph.GetReport().ToText();
            EXPECT_EQ(FrameResources->GetParameterSetCount(), 4u);
        }
        Submit(*command);
        auto* mapped0 = static_cast<const uint32_t*>(firstReadback->Map(0, 4));
        auto* mapped1 = static_cast<const uint32_t*>(secondReadback->Map(0, 4));
        ASSERT_NE(mapped0, nullptr);
        ASSERT_NE(mapped1, nullptr);
        firstReadback->InvalidateMappedRange({0, 4});
        secondReadback->InvalidateMappedRange({0, 4});
        EXPECT_EQ(*mapped0, firstValue + 40);
        EXPECT_EQ(*mapped1, secondValue + 40);
        firstReadback->Unmap();
        secondReadback->Unmap();
    }
}

TEST_P(RenderGraphTest, PixelRasterUavTextureAndBufferFeedCompute) {
    auto& device = *DeviceContext.Device;
    if (!device.GetCapabilities().Features.UavWriteStages.HasFlag(render::ShaderStage::Pixel)) {
        GTEST_SKIP() << "Backend does not expose pixel-stage UAV writes";
    }
    constexpr std::string_view rasterSource = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(3, 1) RWTexture2D<float4> StorageImage : register(u3, space1);
VK_BINDING(4, 1) RWStructuredBuffer<uint> StorageBuffer : register(u4, space1);
[shader("vertex")]
float4 RasterUavVS(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2 - 1, 0, 1);
}
[shader("pixel")]
float4 RasterUavPS(float4 position : SV_Position) : SV_Target0 {
    StorageImage[int2(position.xy)] = float4(.25, .5, .75, 1);
    StorageBuffer[0] = 40;
    return 0;
}
)hlsl";
    constexpr std::string_view computeSource = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 2) Texture2D<float4> StorageImage : register(t0, space2);
VK_BINDING(1, 2) RWStructuredBuffer<uint> StorageBuffer : register(u1, space2);
VK_BINDING(2, 2) RWStructuredBuffer<uint> Result : register(u2, space2);
VK_BINDING(3, 2) SamplerState PointSampler : register(s3, space2);
[shader("compute")][numthreads(1, 1, 1)]
void ConsumeRasterUav() {
    Result[0] = StorageBuffer[0] +
        (uint)round(StorageImage.SampleLevel(PointSampler, float2(.5, .5), 0).g * 100.0f);
}
)hlsl";
    auto rasterProgram = test::CompileFoundationGraphics(device, rasterSource);
    auto computeProgram = test::CompileFoundationCompute(device, computeSource);
    ASSERT_TRUE(rasterProgram);
    ASSERT_TRUE(computeProgram);
    auto readback = device.CreateBuffer({4, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(readback);
    RenderExternalBuffer hostExternal{
        readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};

    auto graph = MakeGraph("pixel raster UAV");
    const auto attachment = graph.CreateTexture(GraphColor(), "attachment");
    auto storageDesc = GraphColor();
    storageDesc.Usage = render::TextureUse::UnorderedAccess | render::TextureUse::Resource;
    const auto storageImage = graph.CreateTexture(storageDesc, "storage image");
    const auto storageBuffer = graph.CreateBuffer(
        {4, render::MemoryType::Device, render::BufferUse::UnorderedAccess, {}},
        "storage buffer");
    const auto result = graph.CreateBuffer(
        {4, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::CopySource, {}},
        "result");
    const auto host = graph.NextVersion(graph.ImportBuffer(
        hostExternal, "readback", RenderGraphExternalAccess::ObservableOutput));
    struct RasterData {
        ShaderProgram* Program{nullptr};
        RgParameterSetHandle Parameters;
        render::RenderBackend Backend{render::RenderBackend::D3D12};
    };
    graph.AddRasterPass<RasterData>(
        "pixel UAV writes",
        [&](RasterData& data, RenderGraphRasterBuilder& builder) {
            const array<RgParameterBinding, 2> bindings{{{.Declaration = "StorageImage",
                                                          .Value = RgTextureParameterBinding{
                                                              .Texture = storageImage,
                                                              .Access = RgParameterAccess::Write}},
                                                         {.Declaration = "StorageBuffer", .Value = RgBufferParameterBinding{.Buffer = storageBuffer, .Range = {0, 4}, .StructureByteStride = 4, .Access = RgParameterAccess::Write}}}};
            data = {
                rasterProgram.Get(),
                builder.CreateParameterSet(*rasterProgram.Get(), 1, bindings),
                device.GetBackend()};
            builder.SetColorAttachment(0, attachment);
        },
        +[](const RasterData& data, RenderGraphRasterContext& context) {
            MaterialPipelineState material;
            material.Primitive.Cull = render::CullMode::None;
            const auto pso = data.Program->GetOrCreateGraphicsPipelineState(
                material, {}, PrimitiveTopology::TriangleList, context.PassState());
            ASSERT_TRUE(pso);
            context.Encoder().BindGraphicsPipelineState(pso.Get());
            context.BindParameterSet(data.Parameters);
            context.Encoder().SetViewport(MakeViewport(data.Backend, 16, 16));
            context.Encoder().SetScissor({0, 0, 16, 16});
            context.Encoder().Draw(3, 1, 0, 0);
        });
    struct ComputeData {
        RgComputeProgramHandle Program;
        RgParameterSetHandle Parameters;
    };
    graph.AddComputePass<ComputeData>(
        "consume pixel UAV",
        [&](ComputeData& data, RenderGraphComputeBuilder& builder) {
            const array<RgParameterBinding, 4> bindings{{{.Declaration = "StorageImage",
                                                          .Value = RgTextureParameterBinding{.Texture = storageImage}},
                                                         {.Declaration = "StorageBuffer",
                                                          .Value = RgBufferParameterBinding{
                                                              .Buffer = storageBuffer,
                                                              .Range = {0, 4},
                                                              .StructureByteStride = 4,
                                                              .Access = RgParameterAccess::Read}},
                                                         {.Declaration = "Result", .Value = RgBufferParameterBinding{.Buffer = result, .Range = {0, 4}, .StructureByteStride = 4, .Access = RgParameterAccess::Write}},
                                                         {.Declaration = "PointSampler", .Value = RgSamplerParameterBinding{}}}};
            data = {
                builder.UseComputeProgram(*computeProgram.Get()),
                builder.CreateParameterSet(*computeProgram.Get(), 2, bindings)};
        },
        +[](const ComputeData& data, RenderGraphComputeContext& context) {
            context.BindComputeProgram(data.Program);
            context.BindParameterSet(data.Parameters);
            context.Encoder().Dispatch(1, 1, 1);
        });
    graph.AddCopyBufferPass("readback", result, host, 4);
    graph.AddComputePass<EmptyPass>(
        "host visibility",
        [=](EmptyPass&, RenderGraphComputeBuilder& builder) {
            builder.ReadBuffer(host, RgBufferAccess::HostRead);
            builder.SetSideEffect();
        },
        EmptyCompute);
    auto command = device.CreateCommandBuffer(DeviceContext.Queue);
    ASSERT_TRUE(command);
    command->Begin();
    ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, *command).Success)
        << graph.GetReport().ToText();
    Submit(*command);
    auto* mapped = static_cast<const uint32_t*>(readback->Map(0, 4));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange({0, 4});
    EXPECT_EQ(*mapped, 90u);
    readback->Unmap();
    EXPECT_GE(graph.GetReport().UavBarriers, 1u);
}

TEST_P(RenderGraphTest, VertexRasterUavWritesWhenSupported) {
    auto& device = *DeviceContext.Device;
    if (!device.GetCapabilities().Features.UavWriteStages.HasFlag(render::ShaderStage::Vertex)) {
        GTEST_SKIP() << "Backend does not expose vertex-stage UAV writes";
    }
    constexpr std::string_view source = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 1) RWStructuredBuffer<uint> VertexOutput : register(u0, space1);
[shader("vertex")]
float4 VertexUavVS(uint id : SV_VertexID) : SV_Position {
    VertexOutput[0] = 17;
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2 - 1, 0, 1);
}
[shader("pixel")]
float4 VertexUavPS() : SV_Target0 { return 0; }
)hlsl";
    auto program = test::CompileFoundationGraphics(device, source);
    ASSERT_TRUE(program);
    auto readback = device.CreateBuffer({4, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(readback);
    RenderExternalBuffer hostExternal{
        readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    auto graph = MakeGraph("vertex raster UAV");
    const auto attachment = graph.CreateTexture(GraphColor(), "attachment");
    auto output = graph.CreateBuffer(
        {4, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::CopySource, {}},
        "vertex output");
    const auto host = graph.NextVersion(graph.ImportBuffer(
        hostExternal, "readback", RenderGraphExternalAccess::ObservableOutput));
    struct Data {
        ShaderProgram* Program{nullptr};
        RgParameterSetHandle Parameters;
        render::RenderBackend Backend{render::RenderBackend::D3D12};
    };
    graph.AddRasterPass<Data>(
        "vertex UAV write",
        [&](Data& data, RenderGraphRasterBuilder& builder) {
            const array<RgParameterBinding, 1> bindings{{{.Declaration = "VertexOutput",
                                                          .Value = RgBufferParameterBinding{
                                                              .Buffer = output,
                                                              .Range = {0, 4},
                                                              .StructureByteStride = 4,
                                                              .Access = RgParameterAccess::Write}}}};
            data = {
                program.Get(), builder.CreateParameterSet(*program.Get(), 1, bindings),
                device.GetBackend()};
            builder.SetColorAttachment(0, attachment);
        },
        +[](const Data& data, RenderGraphRasterContext& context) {
            MaterialPipelineState material;
            material.Primitive.Cull = render::CullMode::None;
            const auto pso = data.Program->GetOrCreateGraphicsPipelineState(
                material, {}, PrimitiveTopology::TriangleList, context.PassState());
            ASSERT_TRUE(pso);
            context.Encoder().BindGraphicsPipelineState(pso.Get());
            context.BindParameterSet(data.Parameters);
            context.Encoder().SetViewport(MakeViewport(data.Backend, 16, 16));
            context.Encoder().SetScissor({0, 0, 16, 16});
            context.Encoder().Draw(3, 1, 0, 0);
        });
    graph.AddCopyBufferPass("readback", output, host, 4);
    graph.AddComputePass<EmptyPass>(
        "host visibility",
        [=](EmptyPass&, RenderGraphComputeBuilder& builder) {
            builder.ReadBuffer(host, RgBufferAccess::HostRead);
            builder.SetSideEffect();
        },
        EmptyCompute);
    auto command = device.CreateCommandBuffer(DeviceContext.Queue);
    ASSERT_TRUE(command);
    command->Begin();
    ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, *command).Success)
        << graph.GetReport().ToText();
    Submit(*command);
    auto* mapped = static_cast<const uint32_t*>(readback->Map(0, 4));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange({0, 4});
    EXPECT_EQ(*mapped, 17u);
    readback->Unmap();
}

TEST_P(RenderGraphTest, ParameterValidationRejectsMissingWrongAndOutOfRangeBindings) {
    auto& device = *DeviceContext.Device;
    constexpr std::string_view source = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(3, 7) Texture2D<float4> Inputs[2] : register(t3, space7);
VK_BINDING(8, 7) RWStructuredBuffer<uint> Output : register(u8, space7);
VK_BINDING(9, 7) SamplerState InputSampler : register(s9, space7);
[shader("compute")][numthreads(1, 1, 1)]
void ValidateBindings() {
    Output[0] = (uint)round(
        (Inputs[0].SampleLevel(InputSampler, float2(.5, .5), 0).r +
         Inputs[1].Load(int3(0, 0, 0)).g) * 100.0f);
}
)hlsl";
    auto program = test::CompileFoundationCompute(device, source);
    ASSERT_TRUE(program);

    const array<std::string_view, 9> expectedCodes{
        "MissingParameterBinding",
        "ParameterDeclaration",
        "ParameterGroup",
        "ParameterType",
        "ParameterArrayElement",
        "ParameterBufferLayout",
        "ParameterGroup",
        "ParameterBufferLayout",
        "ParameterBufferRange"};
    for (uint32_t scenario = 0; scenario < expectedCodes.size(); ++scenario) {
        SCOPED_TRACE(scenario);
        auto graph = MakeGraph("parameter validation");
        auto inputDesc = GraphColor();
        inputDesc.Usage = render::TextureUse::Resource;
        auto input = graph.CreateTexture(inputDesc, "input");
        auto output = graph.CreateBuffer(
            {32, render::MemoryType::Device, render::BufferUse::UnorderedAccess, {}},
            "output");
        graph.AddComputePass<EmptyPass>(
            "invalid parameters",
            [&](EmptyPass&, RenderGraphComputeBuilder& builder) {
                vector<RgParameterBinding> bindings{
                    {.Declaration = "Inputs", .ArrayElement = 0, .Value = RgTextureParameterBinding{.Texture = input}},
                    {.Declaration = "Inputs", .ArrayElement = 1, .Value = RgTextureParameterBinding{.Texture = input}},
                    {.Declaration = "Output",
                     .Value = RgBufferParameterBinding{
                         .Buffer = output, .Range = {0, 4}, .StructureByteStride = 4, .Access = RgParameterAccess::Write}},
                    {.Declaration = "InputSampler", .Value = RgSamplerParameterBinding{}}};
                uint32_t group = 7;
                switch (scenario) {
                    case 0:
                        bindings.erase(bindings.begin() + 1);
                        break;
                    case 1:
                        bindings[0].Declaration = "Unknown";
                        break;
                    case 2:
                        group = 6;
                        break;
                    case 3:
                        bindings[0].Value = RgBufferParameterBinding{
                            .Buffer = output, .Range = {0, 4}, .StructureByteStride = 4};
                        break;
                    case 4:
                        bindings[1].ArrayElement = 2;
                        break;
                    case 5:
                        bindings[2].Value = RgBufferParameterBinding{
                            .Buffer = output, .Range = {0, 4}, .Access = RgParameterAccess::Write};
                        break;
                    case 6:
                        group = 11;
                        bindings.clear();
                        break;
                    case 7: {
                        const uint64_t storageAlignment = std::max<uint64_t>(
                            1, device.GetCapabilities().Limits.StorageBufferOffsetAlignment);
                        const uint64_t misalignedOffset = storageAlignment > 4 ? 4 : 2;
                        bindings[2].Value = RgBufferParameterBinding{
                            .Buffer = output,
                            .Range = {misalignedOffset, 4},
                            .StructureByteStride = 4,
                            .Access = RgParameterAccess::Write};
                        break;
                    }
                    case 8:
                        bindings[2].Value = RgBufferParameterBinding{
                            .Buffer = output,
                            .Range = {32, 4},
                            .StructureByteStride = 4,
                            .Access = RgParameterAccess::Write};
                        break;
                    default:
                        break;
                }
                EXPECT_FALSE(builder.CreateParameterSet(*program.Get(), group, bindings).IsValid());
                builder.SetSideEffect();
            },
            EmptyCompute);
        EXPECT_FALSE(graph.Compile());
        const auto found = std::find_if(
            graph.GetReport().Diagnostics.begin(), graph.GetReport().Diagnostics.end(),
            [&](const RenderGraphDiagnostic& diagnostic) {
                return diagnostic.Code == expectedCodes[scenario];
            });
        EXPECT_NE(found, graph.GetReport().Diagnostics.end()) << graph.GetReport().ToText();
        EXPECT_EQ(FrameResources->GetPoolStats().Created, 0u);
    }
}

TEST_P(RenderGraphTest, OwnedUploadReadbackAndResourceReuseFollowFenceReceipts) {
    auto& device = *DeviceContext.Device;
    for (const bool optimize : {false, true}) {
        BeginFlight(optimize ? 102 : 101);
        auto command = device.CreateCommandBuffer(DeviceContext.Queue);
        ASSERT_TRUE(command);
        command->Begin();
        RgReadbackTicket bytesTicket, firstImage, secondImage;
        shared_ptr<FrameSubmission> receipt;
        weak_ptr<int> lifetime;
        {
            auto graph = MakeGraph();
            graph.SetCompileOptions({.ReuseResources = optimize, .MergeRasterPasses = optimize, .OptimizeAttachmentStores = optimize, .EliminateBarriers = optimize, .BatchBarriers = optimize});
            auto owner = make_shared<int>(123);
            lifetime = owner;
            graph.Retain(owner);
            const array<uint32_t, 4> bytes{19, 73, 0x12345678, 0xffffffff};
            const auto upload = graph.UploadBuffer("upload", std::as_bytes(std::span{bytes}), render::BufferUse::CopySource);
            graph.UploadBuffer("dead upload", std::as_bytes(std::span{bytes}), render::BufferUse::CopySource);
            bytesTicket = graph.ReadbackBuffer("owned readback", upload);
            auto a = graph.CreateTexture(GraphColor(), "a");
            Clear(graph, a, "clear a");
            Clear(graph, a, "preserve a", render::LoadAction::Load);
            firstImage = graph.ReadbackTexture("read a", a);
            const auto b = graph.CreateTexture(GraphColor(), "b");
            graph.AddRasterPass<EmptyPass>("clear b", [=](EmptyPass&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, b, {.Clear = {{1, 0, 0, 1}}}); }, EmptyRaster);
            secondImage = graph.ReadbackTexture("read b", b);
            EXPECT_EQ(bytesTicket.Status(), FrameOperationStatus::Declared);
            const auto result = RenderGraphTestDriver::Execute(graph, *command);
            ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
            receipt = result.Submission;
            EXPECT_FALSE(graph.GetReport().Passes[1].Live);
            EXPECT_EQ(graph.GetReport().ReusedResources, optimize ? 1u : 0u);
            EXPECT_EQ(graph.GetReport().MergedRasterPasses, optimize ? 1u : 0u);
            EXPECT_EQ(bytesTicket.Status(), FrameOperationStatus::Recorded);
            vector<byte> premature;
            EXPECT_FALSE(bytesTicket.Read(premature));
        }
        EXPECT_FALSE(lifetime.expired());
        Writes.Flush(device);
        command->End();
        auto* native = command.Get();
        DeviceContext.Queue->Submit({.CmdBuffers = std::span{&native, 1}});
        ASSERT_TRUE(receipt->Submit(receipt->Serial()));
        EXPECT_EQ(bytesTicket.Status(), FrameOperationStatus::Submitted);
        vector<byte> premature;
        EXPECT_FALSE(bytesTicket.Read(premature));
        EXPECT_FALSE(receipt->Complete(receipt->Serial() + 1, true));
        DeviceContext.Queue->Wait();
        ASSERT_TRUE(receipt->Complete(receipt->Serial(), true));
        EXPECT_TRUE(lifetime.expired());
        vector<byte> actual, imageA, imageB;
        ASSERT_TRUE(bytesTicket.Read(actual));
        ASSERT_TRUE(firstImage.Read(imageA));
        ASSERT_TRUE(secondImage.Read(imageB));
        array<uint32_t, 4> unpacked;
        std::memcpy(unpacked.data(), actual.data(), sizeof(unpacked));
        EXPECT_EQ(unpacked, (array<uint32_t, 4>{19, 73, 0x12345678, 0xffffffff}));
        EXPECT_NEAR(uint8_t(imageA[0]), 64, 1);
        EXPECT_NEAR(uint8_t(imageA[1]), 128, 1);
        EXPECT_NEAR(uint8_t(imageA[2]), 191, 1);
        EXPECT_EQ(uint8_t(imageB[0]), 255);
        EXPECT_EQ(uint8_t(imageB[1]), 0);
        EXPECT_EQ(uint8_t(imageB[2]), 0);
    }
}

TEST_P(RenderGraphTest, DetachedSubmissionKeepsPayloadAndPublishesDuplicateImportsAfterWorkspaceReuse) {
    auto& device = *DeviceContext.Device;
    auto texture = device.CreateTexture(GraphColor(2));
    auto command = device.CreateCommandBuffer(DeviceContext.Queue);
    ASSERT_TRUE(texture);
    ASSERT_TRUE(command);
    array<render::TextureStates, 2> states{render::TextureState::Undefined, render::TextureState::Undefined};
    array<render::TextureStates, 2> duplicateStates = states;
    array<uint8_t, 2> valid{}, duplicateValid{};
    auto external = make_unique<RenderExternalTexture>(RenderExternalTexture{texture.Get(), texture->GetDesc(), states, valid});
    auto duplicate = make_unique<RenderExternalTexture>(RenderExternalTexture{texture.Get(), texture->GetDesc(), duplicateStates, duplicateValid});
    weak_ptr<int> ownerLifetime, payloadLifetime;
    shared_ptr<FrameSubmission> receipt;
    RgOperationTicket ticket;
    command->Begin();
    {
        auto graph = MakeGraph("detached state snapshot");
        auto owner = make_shared<int>(17);
        auto payloadOwner = make_shared<int>(23);
        ownerLifetime = owner;
        payloadLifetime = payloadOwner;
        graph.Retain(owner);
        auto value = graph.ImportTexture(*external, "external", RenderGraphExternalAccess::ObservableOutput);
        EXPECT_EQ(value, graph.ImportTexture(*duplicate, "duplicate", RenderGraphExternalAccess::ObservableOutput));
        value = graph.NextVersion(value);
        struct Payload { shared_ptr<int> Owner; };
        const auto pass = graph.AddRasterPass<Payload>("write mip one", [&](Payload& data, RenderGraphRasterBuilder& builder) {
            data.Owner = payloadOwner;
            builder.SetColorAttachment(0, value, {.View = {.Range = {0, 1, 1, 1}}, .Clear = {{.25f, .5f, .75f, 1}}});
        }, +[](const Payload& data, RenderGraphRasterContext&) { EXPECT_EQ(*data.Owner, 23); });
        ticket = graph.Track(pass);
        const auto result = RenderGraphTestDriver::Execute(graph, *command);
        ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
        receipt = result.Submission;
    }
    EXPECT_FALSE(ownerLifetime.expired());
    EXPECT_FALSE(payloadLifetime.expired());
    EXPECT_EQ(ticket.Status(), FrameOperationStatus::Recorded);
    {
        auto unrelated = MakeGraph("reuse CPU workspace before prior submission");
        auto color = unrelated.CreateTexture(GraphColor(), "unrelated");
        Clear(unrelated, color, "other topology", render::LoadAction::Clear, render::StoreAction::Store, true);
        ASSERT_TRUE(unrelated.Compile()) << unrelated.GetReport().ToText();
    }
    EXPECT_EQ(states[1], render::TextureState::Undefined);
    EXPECT_EQ(valid, (array<uint8_t, 2>{0, 0}));
    Writes.Flush(device);
    command->End();
    auto* native = command.Get();
    DeviceContext.Queue->Submit({.CmdBuffers = std::span{&native, 1}});
    ASSERT_TRUE(receipt->Submit(receipt->Serial()));
    EXPECT_EQ(states, (array<render::TextureStates, 2>{render::TextureState::Undefined, render::TextureState::RenderTarget}));
    EXPECT_EQ(duplicateStates, states);
    EXPECT_EQ(valid, (array<uint8_t, 2>{0, 1}));
    EXPECT_EQ(duplicateValid, valid);
    EXPECT_TRUE(external->Written);
    EXPECT_TRUE(duplicate->Written);
    EXPECT_EQ(ticket.Status(), FrameOperationStatus::Submitted);
    EXPECT_FALSE(ownerLifetime.expired());
    EXPECT_FALSE(payloadLifetime.expired());
    external.reset();
    duplicate.reset();
    DeviceContext.Queue->Wait();
    ASSERT_TRUE(receipt->Complete(receipt->Serial(), true));
    EXPECT_EQ(ticket.Status(), FrameOperationStatus::GpuCompleted);
    EXPECT_TRUE(ownerLifetime.expired());
    EXPECT_TRUE(payloadLifetime.expired());
    RenderGraphTestDriver::Completed(native);
}

TEST_P(RenderGraphTest, DetachedFailedPrefixCommitsOnlyOnSubmitAndCancelsUnrecordedTickets) {
    auto& device = *DeviceContext.Device;
    for (const bool cancel : {true, false}) {
        BeginFlight(cancel ? 201 : 202);
        auto texture = device.CreateTexture(GraphColor(2));
        auto command = device.CreateCommandBuffer(DeviceContext.Queue);
        ASSERT_TRUE(texture);
        ASSERT_TRUE(command);
        array<render::TextureStates, 2> states{render::TextureState::Undefined, render::TextureState::Undefined};
        array<uint8_t, 2> valid{};
        RenderExternalTexture external{texture.Get(), texture->GetDesc(), states, valid};
        shared_ptr<FrameSubmission> receipt;
        RgOperationTicket recorded, failed, culled;
        weak_ptr<int> lifetime;
        command->Begin();
        {
            auto graph = MakeGraph("detached partial recording");
            auto owner = make_shared<int>(42);
            lifetime = owner;
            graph.Retain(owner);
            auto value = graph.ImportTexture(external, "output", RenderGraphExternalAccess::ObservableOutput);
            recorded = graph.Track(Clear(graph, value, "recorded mip zero", render::LoadAction::Clear, render::StoreAction::Store, false, 0));
            failed = graph.Track(Clear(graph, value, "failed mip one", render::LoadAction::Clear, render::StoreAction::Store, false, 1));
            culled = graph.Track(graph.AddComputePass<EmptyPass>("unobservable", [](EmptyPass&, RenderGraphComputeBuilder&) {}, EmptyCompute));
            test::FailingGraphCommand failing(*command);
            failing.PassesBeforeFailure = 1;
            const auto result = RenderGraphTestDriver::Execute(graph, failing, command.Get());
            EXPECT_FALSE(result.Success);
            EXPECT_TRUE(result.CommandsRecorded);
            EXPECT_EQ(graph.GetReport().Diagnostics.front().Code, "BeginRenderPass");
            receipt = result.Submission;
        }
        ASSERT_TRUE(receipt);
        EXPECT_FALSE(lifetime.expired());
        EXPECT_EQ(recorded.Status(), FrameOperationStatus::Recorded);
        EXPECT_EQ(failed.Status(), FrameOperationStatus::Declared);
        EXPECT_EQ(culled.Status(), FrameOperationStatus::Declared);
        EXPECT_EQ(valid, (array<uint8_t, 2>{0, 0}));
        EXPECT_EQ(states[0], render::TextureState::Undefined);
        command->End();
        auto* native = command.Get();
        if (cancel) {
            receipt->Cancel();
            EXPECT_EQ(recorded.Status(), FrameOperationStatus::Cancelled);
            EXPECT_EQ(states, (array<render::TextureStates, 2>{render::TextureState::Undefined, render::TextureState::Undefined}));
            EXPECT_EQ(valid, (array<uint8_t, 2>{0, 0}));
            EXPECT_FALSE(external.Written);
        } else {
            DeviceContext.Queue->Submit({.CmdBuffers = std::span{&native, 1}});
            ASSERT_TRUE(receipt->Submit(receipt->Serial()));
            EXPECT_EQ(states, (array<render::TextureStates, 2>{render::TextureState::RenderTarget, render::TextureState::RenderTarget}));
            EXPECT_EQ(valid, (array<uint8_t, 2>{1, 0}));
            EXPECT_TRUE(external.Written);
            EXPECT_EQ(recorded.Status(), FrameOperationStatus::Submitted);
            EXPECT_FALSE(lifetime.expired());
            DeviceContext.Queue->Wait();
            ASSERT_TRUE(receipt->Complete(receipt->Serial(), false));
            EXPECT_EQ(recorded.Status(), FrameOperationStatus::Cancelled);
        }
        EXPECT_EQ(failed.Status(), FrameOperationStatus::Cancelled);
        EXPECT_EQ(culled.Status(), FrameOperationStatus::Cancelled);
        EXPECT_TRUE(lifetime.expired());
        RenderGraphTestDriver::Completed(native);
    }
}

TEST_P(RenderGraphTest, PhysicalAccessPlanAggregatesRangesAndPreservesAccumulatedReadStages) {
    auto& device = *DeviceContext.Device;
    auto buffer = device.CreateBuffer({64, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::Resource, {}});
    auto command = device.CreateCommandBuffer(DeviceContext.Queue);
    ASSERT_TRUE(buffer);
    ASSERT_TRUE(command);
    RenderExternalBuffer external{buffer.Get(), buffer->GetDesc(), render::BufferState::Undefined};
    auto graph = MakeGraph("physical access summary");
    auto value = graph.NextVersion(graph.ImportBuffer(external, "ranges", RenderGraphExternalAccess::ObservableOutput));
    graph.AddComputePass<EmptyPass>("write two ranges", [=](EmptyPass&, RenderGraphComputeBuilder& builder) {
        builder.WriteBuffer(value, RgBufferAccess::UnorderedAccess, {0, 32});
        builder.WriteBuffer(value, RgBufferAccess::UnorderedAccess, {32, 32});
    }, EmptyCompute);
    graph.AddComputePass<EmptyPass>("compute reads", [=](EmptyPass&, RenderGraphComputeBuilder& builder) {
        builder.ReadBuffer(value, RgBufferAccess::ShaderRead, {0, 32});
        builder.ReadBuffer(value, RgBufferAccess::ShaderRead, {32, 32});
        builder.SetSideEffect();
    }, EmptyCompute);
    const auto color = graph.CreateTexture(GraphColor(), "raster attachment");
    graph.AddRasterPass<EmptyPass>("graphics reads", [=](EmptyPass&, RenderGraphRasterBuilder& builder) {
        builder.SetColorAttachment(0, color);
        builder.ReadBuffer(value, RgBufferAccess::ShaderRead, {0, 64});
        builder.SetSideEffect();
    }, EmptyRaster);
    value = graph.NextVersion(value);
    graph.AddComputePass<EmptyPass>("overwrite two ranges", [=](EmptyPass&, RenderGraphComputeBuilder& builder) {
        builder.WriteBuffer(value, RgBufferAccess::UnorderedAccess, {0, 32});
        builder.WriteBuffer(value, RgBufferAccess::UnorderedAccess, {32, 32});
    }, EmptyCompute);
    command->Begin();
    test::FailingGraphCommand recording(*command);
    recording.PassesBeforeFailure = UINT32_MAX;
    ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, recording, command.Get()).Success) << graph.GetReport().ToText();
    vector<render::BarrierBufferDescriptor> barriers;
    for (const auto& recorded : recording.RecordedBarriers)
        if (const auto* barrier = std::get_if<render::BarrierBufferDescriptor>(&recorded); barrier && barrier->Target == buffer.Get())
            barriers.push_back(*barrier);
    ASSERT_EQ(barriers.size(), 3u);
    EXPECT_EQ(barriers[0].Before, render::BufferState::Undefined);
    EXPECT_EQ(barriers[0].After, render::BufferState::UnorderedAccess);
    EXPECT_EQ(barriers[1].Before, render::BufferState::UnorderedAccess);
    EXPECT_EQ(barriers[1].After, render::BufferState::ShaderRead);
    EXPECT_EQ(barriers[2].Before, render::BufferState::ShaderRead);
    EXPECT_EQ(barriers[2].After, render::BufferState::UnorderedAccess);
    EXPECT_EQ(barriers[2].BeforeStages, render::ShaderStage::Compute | render::ShaderStage::Graphics);
    EXPECT_EQ(barriers[2].AfterStages, render::ShaderStage::Compute);
    Submit(*command);
}

TEST_P(RenderGraphTest, DepthAndStencilViewsSampleTheirSelectedAspects) {
    auto& device = *DeviceContext.Device;
    auto program = test::CompileFoundationCompute(device, R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0,0) Texture2D<float> Depth : register(t0);
VK_BINDING(1,0) Texture2D<uint> Stencil : register(t1);
VK_BINDING(2,0) RWStructuredBuffer<uint2> Result : register(u0);
[shader("compute")][numthreads(1,1,1)] void CSMain() { Result[0] = uint2((uint)round(Depth.Load(int3(0,0,0)) * 1000), Stencil.Load(int3(0,0,0))); }
)hlsl");
    ASSERT_TRUE(program);
    auto graph = MakeGraph();
    auto desc = GraphColor();
    desc.Format = render::TextureFormat::D24_UNORM_S8_UINT;
    desc.Usage = render::TextureUse::DepthStencilWrite | render::TextureUse::Resource;
    if (!render::ValidateTextureDescriptor(desc, device).Supported) GTEST_SKIP() << "Sampled depth/stencil format unavailable";
    const auto depth = graph.CreateTexture(desc, "depth/stencil");
    graph.AddRasterPass<EmptyPass>("clear aspects", [=](EmptyPass&, RenderGraphRasterBuilder& b) { b.SetDepthAttachment(depth, {.Clear = {.25f, 7}}); }, EmptyRaster);
    const auto buffer = graph.CreateBuffer({8, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::CopySource, {}}, "values");
    struct Data {
        RgComputeProgramHandle Program;
        RgParameterSetHandle Set;
    };
    graph.AddComputePass<Data>("sample aspects", [&](Data& data, RenderGraphComputeBuilder& b) {
        const RgParameterBinding bindings[]{
            {"Depth", 0, RgTextureParameterBinding{depth, {.Range = {0,1,0,1,render::TextureAspect::Depth}}}},
            {"Stencil", 0, RgTextureParameterBinding{depth, {.Range = {0,1,0,1,render::TextureAspect::Stencil}}}},
            {"Result", 0, RgBufferParameterBinding{buffer, {0,8}, 8, render::TextureFormat::UNKNOWN, RgParameterAccess::Write}}};
        data = {b.UseComputeProgram(*program), b.CreateParameterSet(*program, 0, bindings)}; }, +[](const Data& data, RenderGraphComputeContext& c) { c.BindComputeProgram(data.Program); c.BindParameterSet(data.Set); c.Encoder().Dispatch(1,1,1); });
    auto ticket = graph.ReadbackBuffer("values", buffer);
    auto command = device.CreateCommandBuffer(DeviceContext.Queue);
    ASSERT_TRUE(command);
    command->Begin();
    ASSERT_TRUE(RenderGraphTestDriver::Execute(graph, *command).Success) << graph.GetReport().ToText();
    Submit(*command);
    vector<byte> bytes;
    ASSERT_TRUE(ticket.Read(bytes));
    array<uint32_t, 2> actual;
    std::memcpy(actual.data(), bytes.data(), sizeof(actual));
    EXPECT_EQ(actual[0], 250u);
    EXPECT_EQ(actual[1], 7u);
}

INSTANTIATE_TEST_SUITE_P(Backends, RenderGraphTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
