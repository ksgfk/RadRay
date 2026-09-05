#include "gpu_test_fixture.h"
#include "render_graph_test_driver.h"
#include "foundation_shader_fixture.h"
#include "failing_graph_command.h"

#include <chrono>
#include <gtest/gtest.h>
#include <radray/utility.h>
#include <radray/runtime/render_framework/viewport.h>

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
class RenderGraphTest : public testing::TestWithParam<render::RenderBackend> {
protected:
    void SetUp() override {
        ASSERT_TRUE(render::test::TryCreateDevice(GetParam(), DeviceContext, true));
        Registry = make_unique<render::RenderPassRegistry>(DeviceContext.Device.get());
        Pool = make_unique<RenderResourcePool>(*DeviceContext.Device, *Registry);
        Pool->BeginFlight(1);
    }
    void TearDown() override {
        DeviceContext.Queue->Wait();
        Pool.reset();
        Registry.reset();
        DeviceContext.Device.reset();
        EXPECT_EQ(DeviceContext.ValidationErrors.load(), 0u);
    }
    RenderGraph MakeGraph(std::string_view name = "test") { return RenderGraph{*DeviceContext.Device, *Pool, *Registry, name}; }
    void Submit(render::CommandBuffer& command) {
        command.End();
        auto* raw = &command;
        DeviceContext.Queue->Submit({.CmdBuffers = std::span{&raw, 1}});
        DeviceContext.Queue->Wait();
    }
    render::test::DeviceContext DeviceContext;
    unique_ptr<render::RenderPassRegistry> Registry;
    unique_ptr<RenderResourcePool> Pool;
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
    auto dst = graph.ImportBuffer(output, "readback", RenderGraphExternalAccess::ObservableOutput);
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
        if (frame > 1) Pool->BeginFlight(frame);
        auto graph = MakeGraph();
        auto color = graph.CreateTexture(GraphColor(2), "mipped");
        auto dst = graph.ImportBuffer(output, "readback", RenderGraphExternalAccess::ObservableOutput);
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
    EXPECT_EQ(Pool->GetStats().Created, 1u);
    EXPECT_EQ(Pool->GetStats().ViewsCreated, 1u);
    EXPECT_EQ(Pool->GetStats().Hits, 101u);
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
        const auto output = graph.ImportTexture(external, "output", RenderGraphExternalAccess::ObservableOutput);
        if (compute)
            graph.AddComputePass<EmptyPass>("fail compute", [=](EmptyPass&, RenderGraphComputeBuilder& builder) { builder.WriteTexture(output); }, EmptyCompute);
        else
            Clear(graph, output, "fail raster");
        auto command = device.CreateCommandBuffer(DeviceContext.Queue);
        ASSERT_TRUE(command);
        command->Begin();
        test::FailingGraphCommand failing(*command);
        const auto result = RenderGraphTestDriver::Execute(graph, failing);
        EXPECT_FALSE(result.Success);
        EXPECT_TRUE(result.CommandsRecorded);
        EXPECT_FALSE(external.Written);
        EXPECT_EQ(valid[0], 0);
        EXPECT_EQ(states[0], compute ? render::TextureState::UnorderedAccess : render::TextureState::RenderTarget);
        EXPECT_FALSE(graph.GetReport().Passes[0].Executed);
        EXPECT_EQ(graph.GetReport().Diagnostics[0].Code, compute ? "BeginComputePass" : "BeginRenderPass");

        const uint64_t row = Align(uint64_t{16 * 4}, device.GetDetail().TextureDataPitchAlignment);
        auto readback = device.CreateBuffer({row * 16, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
        ASSERT_TRUE(readback);
        RenderExternalBuffer bytes{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
        auto recovery = MakeGraph("recovery");
        const auto color = recovery.ImportTexture(external, "output", RenderGraphExternalAccess::ReadWrite);
        const auto dest = recovery.ImportBuffer(bytes, "readback", RenderGraphExternalAccess::ObservableOutput);
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
        Pool->BeginFlight(compute ? 3 : 2);
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
    auto set = device.CreateShaderParameterSet({program->Layout.get(), 0});
    ASSERT_TRUE(set);
    const uint32_t initial = 5;
    auto upload = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{&initial, 1}), render::BufferUse::CopySource);
    auto readback = device.CreateBuffer({4, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(upload);
    ASSERT_TRUE(readback);
    RenderExternalBuffer src{upload.Get(), upload->GetDesc(), render::BufferState::HostWrite, true};
    RenderExternalBuffer dst{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    auto graph = MakeGraph();
    auto input = graph.ImportBuffer(src, "upload", RenderGraphExternalAccess::ReadOnly);
    auto output = graph.ImportBuffer(dst, "output", RenderGraphExternalAccess::ObservableOutput);
    auto buffer = graph.CreateBuffer({4, render::MemoryType::Device, render::BufferUse::CopyDestination | render::BufferUse::CopySource | render::BufferUse::UnorderedAccess, {}}, "uav");
    graph.AddCopyBufferPass("initialize", input, buffer, 4);
    struct Data {
        RgBufferHandle Buffer;
        render::ComputePipelineState* Pso;
        render::ShaderParameterSet* Set;
        render::BindingHandle Binding;
    };
    for (uint32_t p = 0; p < 2; ++p) graph.AddComputePass<Data>("transform", [&](Data& data, RenderGraphComputeBuilder& builder) { data = {builder.ReadWriteBuffer(buffer), program->Pso.get(), set.Get(), program->Layout->FindBinding("Value")}; }, +[](const Data& data, RenderGraphComputeContext& context) {
        EXPECT_TRUE(data.Set->Set(data.Binding, 0, render::ShaderBufferBinding{context.GetBuffer(data.Buffer), {0, 4}, 4}));
        EXPECT_TRUE(data.Set->FlushWrites()); context.Encoder().BindComputePipelineState(data.Pso);
        context.Encoder().BindShaderParameterSet(0, data.Set); context.Encoder().Dispatch(1, 1, 1); });
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
    auto writeSet = device.CreateShaderParameterSet({writer->Layout.get(), 0});
    auto readSet = device.CreateShaderParameterSet({reader->Layout.get(), 0});
    ASSERT_TRUE(buffer);
    ASSERT_TRUE(output);
    ASSERT_TRUE(readback);
    ASSERT_TRUE(writeSet);
    ASSERT_TRUE(readSet);
    EXPECT_TRUE(writeSet->Set(writer->Layout->FindBinding("Value"), 0, render::ShaderBufferBinding{buffer.Get(), {0, 4}, 4}));
    EXPECT_TRUE(readSet->Set(reader->Layout->FindBinding("Value"), 0, render::ShaderBufferBinding{buffer.Get(), {0, 4}, 4}));
    EXPECT_TRUE(readSet->Set(reader->Layout->FindBinding("Output"), 0, render::ShaderBufferBinding{output.Get(), {0, 4}, 4}));
    ASSERT_TRUE(writeSet->FlushWrites());
    ASSERT_TRUE(readSet->FlushWrites());
    RenderExternalBuffer shared{buffer.Get(), buffer->GetDesc(), render::BufferState::Undefined};
    RenderExternalBuffer result{output.Get(), output->GetDesc(), render::BufferState::Undefined};
    RenderExternalBuffer bytes{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    struct Data {
        render::ComputePipelineState* Pso;
        render::ShaderParameterSet* Set;
    };
    const auto dispatch = +[](const Data& data, RenderGraphComputeContext& ctx) {
        ctx.Encoder().BindComputePipelineState(data.Pso);
        ctx.Encoder().BindShaderParameterSet(0, data.Set);
        ctx.Encoder().Dispatch(1, 1, 1);
    };
    auto first = MakeGraph("producer");
    const auto destination = first.ImportBuffer(shared, "shared", RenderGraphExternalAccess::ObservableOutput);
    first.AddComputePass<Data>("write", [&](Data& data, RenderGraphComputeBuilder& builder) {
        builder.WriteBuffer(destination); data = {writer->Pso.get(), writeSet.Get()}; }, dispatch);
    auto firstCommand = device.CreateCommandBuffer(DeviceContext.Queue);
    auto secondCommand = device.CreateCommandBuffer(DeviceContext.Queue);
    ASSERT_TRUE(firstCommand);
    ASSERT_TRUE(secondCommand);
    firstCommand->Begin();
    ASSERT_TRUE(RenderGraphTestDriver::Execute(first, *firstCommand).Success);
    firstCommand->End();
    ASSERT_EQ(shared.State, render::BufferState::UnorderedAccess);

    auto second = MakeGraph("consumer");
    const auto source = second.ImportBuffer(shared, "shared", RenderGraphExternalAccess::ReadOnly);
    const auto target = second.ImportBuffer(result, "result", RenderGraphExternalAccess::ReadWrite);
    const auto host = second.ImportBuffer(bytes, "readback", RenderGraphExternalAccess::ObservableOutput);
    second.AddComputePass<Data>("read UAV", [&](Data& data, RenderGraphComputeBuilder& builder) {
        builder.ReadBuffer(source, RgBufferAccess::UnorderedAccess); builder.WriteBuffer(target);
        data = {reader->Pso.get(), readSet.Get()}; }, dispatch);
    second.AddCopyBufferPass("readback", target, host, 4);
    second.AddComputePass<EmptyPass>("host", [=](EmptyPass&, RenderGraphComputeBuilder& builder) {
        builder.ReadBuffer(host, RgBufferAccess::HostRead); builder.SetSideEffect(); }, EmptyCompute);
    secondCommand->Begin();
    ASSERT_TRUE(RenderGraphTestDriver::Execute(second, *secondCommand).Success);
    EXPECT_EQ(second.GetReport().UavBarriers, 1u);
    auto* command = firstCommand.Get();
    DeviceContext.Queue->Submit({.CmdBuffers = std::span{&command, 1}});
    Submit(*secondCommand);
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
    auto sampleSet = device.CreateShaderParameterSet({graphics->GetPipelineLayout(), 0});
    ASSERT_TRUE(sampleSet);
    auto computeSet = device.CreateShaderParameterSet({compute->Layout.get(), 0});
    ASSERT_TRUE(computeSet);
    const uint64_t row = Align(uint64_t{16 * 4}, device.GetDetail().TextureDataPitchAlignment);
    auto readback = device.CreateBuffer({row * 16, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(readback);
    RenderExternalBuffer external{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    for (uint32_t computeFirst = 0; computeFirst < 2; ++computeFirst) {
        if (computeFirst) Pool->BeginFlight(2);
        auto graph = MakeGraph();
        auto desc = GraphColor();
        desc.Usage |= render::TextureUse::UnorderedAccess;
        auto input = graph.CreateTexture(desc, "input"), output = graph.CreateTexture(GraphColor(), "output");
        if (!computeFirst)
            Clear(graph, input, "raster producer");
        else {
            struct Data {
                RgTextureViewHandle View;
                test::FoundationComputeProgram* Program;
                render::ShaderParameterSet* Set;
            };
            graph.AddComputePass<Data>("compute producer", [&](Data& data, RenderGraphComputeBuilder& builder) { data = {builder.WriteTexture(input), &*compute, computeSet.Get()}; }, +[](const Data& data, RenderGraphComputeContext& ctx) {
                EXPECT_TRUE(data.Set->Set(data.Program->Layout->FindBinding("OutputTexture"), 0, ctx.GetTextureView(data.View)));
                EXPECT_TRUE(data.Set->FlushWrites()); ctx.Encoder().BindComputePipelineState(data.Program->Pso.get());
                ctx.Encoder().BindShaderParameterSet(0, data.Set); ctx.Encoder().Dispatch(2, 2, 1); });
        }
        struct Data {
            RgTextureViewHandle View;
            ShaderProgram* Program;
            render::ShaderParameterSet* Set;
            render::RenderBackend Backend;
        };
        for (const auto load : {render::LoadAction::Clear, render::LoadAction::Load}) {
            graph.AddRasterPass<Data>("sample", [&](Data& data, RenderGraphRasterBuilder& builder) {
                data = {builder.ReadTexture(input), graphics.Get(), sampleSet.Get(), device.GetBackend()};
                builder.SetColorAttachment(0, output, {.Load = load}); }, +[](const Data& data, RenderGraphRasterContext& ctx) {
                MaterialPipelineState material; material.Primitive.Cull = render::CullMode::None;
                auto pso = data.Program->GetOrCreateGraphicsPipelineState(material, {}, PrimitiveTopology::TriangleList, ctx.PassState());
                ASSERT_TRUE(pso);
                EXPECT_TRUE(data.Set->Set(data.Program->GetPipelineLayout()->FindBinding("InputTexture"), 0, ctx.GetTextureView(data.View)));
                EXPECT_TRUE(data.Set->FlushWrites()); ctx.Encoder().BindGraphicsPipelineState(pso.Get());
                ctx.Encoder().BindShaderParameterSet(0, data.Set); ctx.Encoder().SetViewport(MakeViewport(data.Backend, 16, 16));
                ctx.Encoder().SetScissor({0, 0, 16, 16}); ctx.Encoder().Draw(3, 1, 0, 0); });
        }
        auto destination = graph.ImportBuffer(external, "readback", RenderGraphExternalAccess::ObservableOutput);
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

INSTANTIATE_TEST_SUITE_P(Backends, RenderGraphTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
