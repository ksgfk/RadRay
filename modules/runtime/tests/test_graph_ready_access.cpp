#include "foundation_graph_fixture.h"
#include "failing_graph_command.h"
#include <radray/scope_guard.h>

namespace radray {
namespace {
class GraphReadyAccessTest : public test::FoundationGraphGpuTest {
protected:
    void ResolveUndeclared(RenderValidationMode mode, bool texture) {
        auto& device = *Context.Device;
        RenderGraph graph{device, *Resources, *Registry, "undeclared native access", {mode, RenderGraphReportMode::Counters, false}};
        struct Probe {
            RgBufferValue Buffer;
            RgTextureViewHandle View;
        };
        if (texture) {
            const auto target = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource, {}}, "target");
            RgTextureViewHandle attachment;
            graph.AddRasterPass<Probe>("view owner", [&](Probe& data, RenderGraphRasterBuilder& builder) { data.View = attachment = builder.SetColorAttachment(0, target); }, +[](const Probe& data, RenderGraphRasterContext& context) { EXPECT_NE(context.GetTextureView(data.View), nullptr); });
            graph.AddComputePass<Probe>("view trespass", [&](Probe& data, RenderGraphComputeBuilder& builder) {
                data.View = attachment;
                // The resource is readable here, but this pass did not declare the owner's RT view.
                builder.ReadTexture(target);
                builder.SetSideEffect(); }, +[](const Probe& data, RenderGraphComputeContext& context) { context.GetTextureView(data.View); });
        } else {
            const render::BufferDescriptor desc{4, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::Resource, {}};
            const auto target = graph.CreateBuffer(desc, "target");
            const auto dependency = graph.CreateBuffer(desc, "dependency");
            graph.AddComputePass<Probe>("buffer owner", [&](Probe& data, RenderGraphComputeBuilder& builder) {
                data.Buffer = builder.WriteBuffer(target);
                builder.WriteBuffer(dependency); }, +[](const Probe& data, RenderGraphComputeContext& context) { EXPECT_NE(context.GetBuffer(data.Buffer), nullptr); });
            graph.AddComputePass<Probe>("buffer trespass", [&](Probe& data, RenderGraphComputeBuilder& builder) {
                data.Buffer = target;
                builder.ReadBuffer(dependency);
                builder.SetSideEffect(); }, +[](const Probe& data, RenderGraphComputeContext& context) { context.GetBuffer(data.Buffer); });
        }
        auto command = device.CreateCommandBuffer(Context.Queue);
        ASSERT_TRUE(command);
        command->Begin();
        RenderGraphTestDriver::Execute(graph, *command);
        command->End();
    }
};

TEST_P(GraphReadyAccessTest, LegacyRecordRejectsUndeclaredNativeAccessInEveryMode) {
    for (const auto mode : {RenderValidationMode::Off, RenderValidationMode::Full, RenderValidationMode::Off}) {
        for (const bool texture : {false, true}) {
            SCOPED_TRACE(fmt::format("mode {} texture {}", uint32_t(mode), texture));
            EXPECT_DEATH(ResolveUndeclared(mode, texture), "");
        }
    }
}

TEST_P(GraphReadyAccessTest, PixelBindingCannotBorrowAnotherMipsShaderStages) {
    auto& device = *Context.Device;
    auto program = test::CompileFoundationGraphics(device, R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) Texture2D<float> Input : register(t0);
[shader("vertex")] float4 VSMain(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
[shader("pixel")] float PSMain() : SV_Target0 { return Input.Load(int3(0, 0, 0)); }
)hlsl");
    ASSERT_TRUE(program);
    auto cleanup = MakeScopeGuard([this]() noexcept { Resources->Clear(); });
    for (uint32_t attempt = 0; attempt < 2; ++attempt) {
        Resources->BeginFlight(attempt + 1, Writes);
        auto graph = MakeGraph("mip shader visibility");
        const auto input = graph.CreateTexture({render::TextureDimension::Dim2D, 8, 8, 1, 2, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource, {}}, "two mips");
        const auto output = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, "sampled mip");
        for (uint32_t mip = 0; mip < 2; ++mip) {
            graph.AddRasterPass<test::EmptyGraphPass>("initialize mip", [&](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, input, {.View = {.Range = {0, 1, mip, 1}}, .Clear = {mip ? .75f : .25f, 0, 0, 0}}); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
        }
        struct Draw {
            ShaderProgram* Program;
            RgTextureViewHandle Input;
            PreparedShaderGroup Set;
            Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
            uint32_t* Prepared;
            render::RenderBackend Backend;
        };
        uint32_t prepared = 0;
        graph.AddRasterPass<Draw>("sample mip zero", [&](Draw& data, RenderGraphRasterBuilder& builder) {
            data.Program = program.Get(); data.Prepared = &prepared; data.Backend = GetParam();
            data.Input = builder.ReadTexture(input, {.Range = {0, 1, 0, 1}, .Stages = attempt ? render::ShaderStage::Pixel : render::ShaderStage::Vertex});
            builder.ReadTexture(input, {.Range = {0, 1, 1, 1}, .Stages = attempt ? render::ShaderStage::Vertex : render::ShaderStage::Pixel});
            builder.SetColorAttachment(0, output); }, +[](Draw& data, RenderGraphPrepareContext& context) {
            ++*data.Prepared;
            const RgParameterBinding binding{"Input", 0, RgTextureParameterBinding{data.Input}};
            data.Set = context.CreateParameterSet(*data.Program, 0, std::span{&binding, 1});
            MaterialPipelineState state;
            state.Primitive.Cull = render::CullMode::None;
            state.DepthStencil.DepthTestEnable = state.DepthStencil.DepthWriteEnable = false;
            data.Pipeline = context.ResolveGraphicsPipeline(*data.Program, state);
            return data.Set.IsValid() && bool(data.Pipeline); }, +[](const Draw& data, RenderGraphRasterContext& context) {
            context.Encoder().BindGraphicsPipelineState(data.Pipeline.Get());
            context.Encoder().BindShaderParameterSet(data.Set);
            context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 4, 4));
            context.Encoder().SetScissor({0, 0, 4, 4});
            context.Encoder().Draw(3, 1, 0, 0); });
        struct Observer {
            uint32_t* Prepared;
        };
        graph.AddComputePass<Observer>("later preparation", [&](Observer& data, RenderGraphComputeBuilder& builder) {
            data.Prepared = &prepared; builder.SetSideEffect(); }, +[](Observer& data, RenderGraphPrepareContext&) {
            ++*data.Prepared; return true; }, +[](const Observer&, RenderGraphComputeContext&) {});
        auto readback = graph.ReadbackTexture("sample result", output);
        if (!attempt) {
            auto command = device.CreateCommandBuffer(Context.Queue);
            ASSERT_TRUE(command);
            command->Begin();
            test::FailingGraphCommand recording{*command};
            const auto result = RenderGraphTestDriver::Execute(graph, recording, command.Get());
            command->End();
            EXPECT_FALSE(result.Success);
            EXPECT_FALSE(result.CommandsRecorded);
            EXPECT_EQ(recording.RecordingCalls, 0u);
            EXPECT_EQ(graph.GetReport().FirstErrorCode, "ParameterUndeclared") << graph.GetReport().ToText();
            EXPECT_EQ(graph.GetReport().ValidateReadyFrameCalls, 1u);
            EXPECT_EQ(Resources->GetParameterSetCount(), 0u);
        } else {
            ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
            vector<byte> bytes;
            ASSERT_TRUE(readback.Read(bytes));
            ASSERT_GE(bytes.size(), 4u);
            float value;
            std::memcpy(&value, bytes.data(), 4);
            EXPECT_FLOAT_EQ(value, .25f);
        }
        EXPECT_EQ(prepared, 2u);
    }
}
INSTANTIATE_TEST_SUITE_P(Backends, GraphReadyAccessTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
