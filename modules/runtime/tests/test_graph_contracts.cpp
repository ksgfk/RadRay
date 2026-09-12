#include "foundation_graph_fixture.h"

namespace radray {
namespace {
class GraphContractTest : public test::FoundationGraphGpuTest {};

render::TextureDescriptor IntegerImage() {
    return {render::TextureDimension::Dim2D, 64, 64, 1, 1, 1, render::TextureFormat::R32_UINT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource, {}};
}

TEST_P(GraphContractTest, G06InitializedPartialCopyPreservesEightSentinelBytes) {
    auto& device = *Context.Device;
    const array<uint32_t, 4> initial{1, 2, 0x12345678, 0xabcdef01};
    const array<uint32_t, 2> replace{17, 31};
    auto source = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{initial}), render::BufferUse::CopySource);
    auto patch = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{replace}), render::BufferUse::CopySource);
    auto readback = device.CreateBuffer({16, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
    ASSERT_TRUE(source);
    ASSERT_TRUE(patch);
    ASSERT_TRUE(readback);
    RenderExternalBuffer a{source.Get(), source->GetDesc(), render::BufferState::HostWrite, true};
    RenderExternalBuffer b{patch.Get(), patch->GetDesc(), render::BufferState::HostWrite, true};
    RenderExternalBuffer c{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    auto graph = MakeGraph("partial copy sentinel");
    auto target = graph.CreateBuffer({16, render::MemoryType::Device, render::BufferUse::CopyDestination | render::BufferUse::CopySource, {}}, "target");
    graph.AddCopyBufferPass("initialize", graph.ImportBuffer(a, "initial", RenderGraphExternalAccess::ReadOnly), target, 16);
    target = graph.NextVersion(target);
    graph.AddCopyBufferPass("replace first eight", graph.ImportBuffer(b, "patch", RenderGraphExternalAccess::ReadOnly), target, 8);
    const auto host = graph.NextVersion(graph.ImportBuffer(c, "readback", RenderGraphExternalAccess::ObservableOutput));
    graph.AddCopyBufferPass("read all", target, host, 16);
    HostRead(graph, host);
    ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
    const auto bytes = Read(*readback);
    array<uint32_t, 4> actual;
    std::memcpy(actual.data(), bytes.data(), sizeof(actual));
    const array<uint32_t, 4> expected{17, 31, initial[2], initial[3]};
    EXPECT_EQ(actual, expected);
}

TEST_P(GraphContractTest, G01RasterComputeRasterMatches64IntegerReferenceAndCullsDeadBranch) {
    auto& device = *Context.Device;
    auto first = test::CompileFoundationGraphics(device, R"hlsl(
[shader("vertex")] float4 VSMain(uint id : SV_VertexID) : SV_Position { return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1); }
[shader("pixel")] float PSMain(float4 p : SV_Position) : SV_Target0 { return floor(p.x); }
)hlsl");
    auto compute = test::CompileFoundationCompute(device, R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) Texture2D<float> Image : register(t0);
VK_BINDING(1, 0) RWStructuredBuffer<uint> Values : register(u0);
[shader("compute")] [numthreads(64, 1, 1)] void CSMain(uint3 id : SV_DispatchThreadID) { Values[id.x] = 2 * (uint)Image.Load(int3(id.x, 0, 0)) + 1; }
)hlsl");
    auto last = test::CompileFoundationGraphics(device, R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) StructuredBuffer<uint> Values : register(t0);
[shader("vertex")] float4 VSMain(uint id : SV_VertexID) : SV_Position { return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1); }
[shader("pixel")] uint PSMain(float4 p : SV_Position) : SV_Target0 { return Values[(uint)p.x]; }
)hlsl");
    ASSERT_TRUE(first);
    ASSERT_TRUE(compute);
    ASSERT_TRUE(last);
    auto graph = MakeGraph("G01 raster compute raster");
    auto inputDesc = IntegerImage();
    inputDesc.Format = render::TextureFormat::R32_FLOAT;
    const auto input = graph.CreateTexture(inputDesc, "raster integers"), output = graph.CreateTexture(IntegerImage(), "final integers");
    const auto buffer = graph.CreateBuffer({64 * 4, render::MemoryType::Device, render::BufferUse::Resource | render::BufferUse::UnorderedAccess, {}}, "64 integers");
    struct Raster {
        ShaderProgram* Program;
        PreparedShaderGroup Set;
        render::RenderBackend Backend;
        RgBufferValue Values;
    };
    const auto draw = +[](const Raster& data, RenderGraphRasterContext& context) {
        MaterialPipelineState state;
        state.Primitive.Cull = render::CullMode::None;
        state.DepthStencil.DepthTestEnable = state.DepthStencil.DepthWriteEnable = false;
        const auto pso = data.Program->GetOrCreateGraphicsPipelineState(state, {}, PrimitiveTopology::TriangleList, context.PassState());
        ASSERT_TRUE(pso);
        context.Encoder().BindGraphicsPipelineState(pso.Get());
        if (data.Set.IsValid()) context.Encoder().BindShaderParameterSet(data.Set);
        context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 64, 64));
        context.Encoder().SetScissor({0, 0, 64, 64});
        context.Encoder().Draw(3, 1, 0, 0);
    };
    graph.AddRasterPass<Raster>("raster producer", [&](Raster& data, RenderGraphRasterBuilder& builder) { data = {first.Get(), {}, GetParam()}; builder.SetColorAttachment(0, input); }, draw);
    struct Compute {
        ShaderProgram* Shader;
        render::ComputePipelineState* Program;
        PreparedShaderGroup Set;
        RgTextureViewHandle Image;
        RgBufferValue Values;
    };
    graph.AddComputePass<Compute>("compute 2i+1",
        [&](Compute& data, RenderGraphComputeBuilder& builder) {
            data.Shader = compute.Get();
            data.Image = builder.ReadTexture(input);
            data.Values = builder.WriteBuffer(buffer, RgBufferAccess::UnorderedAccess, {0, 256}); },
        +[](Compute& data, RenderGraphPrepareContext& ctx) {
            const RgParameterBinding bindings[]{{"Image", 0, RgTextureParameterBinding{data.Image}},
                                                {"Values", 0, RgBufferParameterBinding{data.Values, {0, 256}, 4}}};
            data.Program = ctx.ResolveComputePipeline(*data.Shader).Get();
            data.Set = ctx.CreateParameterSet(*data.Shader, 0, bindings);
            return data.Program != nullptr && data.Set.IsValid(); },
        +[](const Compute& data, RenderGraphComputeContext& context) {
            context.Encoder().BindComputePipelineState(data.Program);
            context.Encoder().BindShaderParameterSet(data.Set);
            context.Encoder().Dispatch(1, 1, 1); });
    graph.AddRasterPass<Raster>("raster consumer",
        [&](Raster& data, RenderGraphRasterBuilder& builder) {
            data.Program = last.Get();
            data.Backend = GetParam();
            data.Values = builder.ReadBuffer(buffer, RgBufferAccess::ShaderRead, {0, 256});
            builder.SetColorAttachment(0, output); },
        +[](Raster& data, RenderGraphPrepareContext& ctx) {
            const RgParameterBinding binding{"Values", 0, RgBufferParameterBinding{data.Values, {0, 256}, 4}};
            data.Set = ctx.CreateParameterSet(*data.Program, 0, std::span{&binding, 1});
            return data.Set.IsValid(); },
        draw);
    const auto unused = graph.CreateTexture(inputDesc, "dead legal texture");
    graph.AddRasterPass<Raster>("dead legal producer", [&](Raster& data, RenderGraphRasterBuilder& builder) { data = {first.Get(), {}, GetParam()}; builder.SetColorAttachment(0, unused); }, draw);
    const auto pitch = Align(uint64_t{64 * 4}, device.GetDetail().TextureDataPitchAlignment);
    auto readback = device.CreateBuffer({pitch * 64, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
    ASSERT_TRUE(readback);
    RenderExternalBuffer external{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    const auto host = graph.NextVersion(graph.ImportBuffer(external, "readback", RenderGraphExternalAccess::ObservableOutput));
    graph.AddCopyTextureToBufferPass("copy", output, host);
    HostRead(graph, host);
    ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
    const auto bytes = Read(*readback);
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x) {
            uint32_t value;
            std::memcpy(&value, bytes.data() + pitch * y + x * 4, 4);
            ASSERT_EQ(value, 2 * x + 1);
        }
    EXPECT_EQ(graph.GetReport().Passes[1].DataDependencies, (vector<uint32_t>{0}));
    EXPECT_EQ(graph.GetReport().Passes[2].DataDependencies, (vector<uint32_t>{1}));
    EXPECT_FALSE(graph.GetReport().Passes[3].Live);
    EXPECT_FALSE(graph.GetReport().Passes[3].Executed);
    EXPECT_EQ(graph.GetReport().CommandCalls.Draw, 2u);
    EXPECT_EQ(graph.GetReport().CommandCalls.Dispatch, 1u);
    EXPECT_EQ(graph.GetReport().CommandCalls.SetPipeline, 3u);
    EXPECT_EQ(graph.GetReport().CommandCalls.SetParameters, 2u);
    EXPECT_EQ(graph.GetReport().Passes[0].CommandCalls.Draw, 1u);
    EXPECT_EQ(graph.GetReport().Passes[1].CommandCalls.Dispatch, 1u);
    EXPECT_EQ(graph.GetReport().Passes[2].CommandCalls.Draw, 1u);
    EXPECT_EQ(graph.GetReport().Passes[3].CommandCalls.Draw, 0u);
    EXPECT_EQ(graph.GetReport().Resources[3].PhysicalId, 0u);
}

TEST_P(GraphContractTest, G02ClearOverwriteAndLoadPreserveObservablePixels) {
    auto& device = *Context.Device;
    for (const auto action : {render::LoadAction::Clear, render::LoadAction::Load}) {
        auto graph = MakeGraph("G02 content versions");
        auto descriptor = IntegerImage();
        descriptor.Format = render::TextureFormat::R32_FLOAT;
        auto texture = graph.CreateTexture(descriptor, "versioned color");
        for (uint32_t p = 0; p < 2; ++p) {
            texture = graph.NextVersion(texture);
            graph.AddRasterPass<test::EmptyGraphPass>(p ? "B" : "A", [=](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, texture, {.Load = p ? action : render::LoadAction::Clear, .Clear = {p ? .75f : .25f, 0, 0, 0}}); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
        }
        const auto pitch = Align(uint64_t{64 * 4}, device.GetDetail().TextureDataPitchAlignment);
        auto readback = device.CreateBuffer({pitch * 64, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
        ASSERT_TRUE(readback);
        RenderExternalBuffer external{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
        const auto host = graph.NextVersion(graph.ImportBuffer(external, "readback", RenderGraphExternalAccess::ObservableOutput));
        graph.AddCopyTextureToBufferPass("observable readback", texture, host);
        HostRead(graph, host);
        ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
        const auto bytes = Read(*readback);
        float value;
        std::memcpy(&value, bytes.data(), 4);
        EXPECT_FLOAT_EQ(value, action == render::LoadAction::Load ? .25f : .75f);
        EXPECT_EQ(graph.GetReport().Passes[0].Executed, action == render::LoadAction::Load);
    }
}

TEST_P(GraphContractTest, G03InterleavedThreeLayersFourMipsHaveIndependentContentsAndLiveness) {
    auto& device = *Context.Device;
    for (const bool all : {false, true}) {
        auto graph = MakeGraph("G03 independent cells");
        const auto texture = graph.CreateTexture({render::TextureDimension::Dim2DArray, 32, 16, 3, 4, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource, {}}, "3 layers 4 mips");
        for (uint32_t p = 0; p < 12; ++p) {
            const uint32_t cell = (p * 5) % 12, layer = cell / 4, mip = cell % 4;
            graph.AddRasterPass<test::EmptyGraphPass>(fmt::format("layer {} mip {}", layer, mip), [=](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, texture, {.View = {.Range = {layer, 1, mip, 1}}, .Clear = {float(16 * layer + mip), 0, 0, 0}}); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
        }
        array<unique_ptr<render::Buffer>, 12> readbacks;
        array<RenderExternalBuffer, 12> imports{};
        for (uint32_t cell = 0; cell < 12; ++cell)
            if (all || cell == 9) {
                const auto mip = cell % 4, layer = cell / 4;
                const auto pitch = Align(uint64_t{32u >> mip} * 4, device.GetDetail().TextureDataPitchAlignment);
                readbacks[cell] = device.CreateBuffer({pitch * (16 >> mip), render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}}).Release();
                ASSERT_TRUE(readbacks[cell]);
                imports[cell] = {readbacks[cell].get(), readbacks[cell]->GetDesc(), render::BufferState::CopyDestination};
                const auto host = graph.NextVersion(graph.ImportBuffer(imports[cell], fmt::format("readback {}", cell), RenderGraphExternalAccess::ObservableOutput));
                graph.AddCopyTextureToBufferPass(fmt::format("copy cell {}", cell), texture, host, {layer, 1, mip, 1});
                HostRead(graph, host);
            }
        ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
        for (uint32_t p = 0; p < 12; ++p) EXPECT_EQ(graph.GetReport().Passes[p].Executed, all || (p * 5) % 12 == 9);
        for (uint32_t cell = 0; cell < 12; ++cell)
            if (readbacks[cell]) {
                const auto bytes = Read(*readbacks[cell]);
                const uint32_t mip = cell % 4, layer = cell / 4;
                const auto pitch = Align(uint64_t{32u >> mip} * 4, device.GetDetail().TextureDataPitchAlignment);
                for (uint32_t y = 0; y < (16u >> mip); ++y)
                    for (uint32_t x = 0; x < (32u >> mip); ++x) {
                        float value;
                        std::memcpy(&value, bytes.data() + pitch * y + x * 4, 4);
                        ASSERT_FLOAT_EQ(value, float(16 * layer + mip));
                    }
            }
    }
}

TEST_P(GraphContractTest, R05GpuGeneratedIndirectDrawIndexedAndDispatchCountZeroOneSeventeen) {
    auto& device = *Context.Device;
    auto producer = test::CompileFoundationCompute(device, R"hlsl(
#include <core/platform.hlsli>
struct Data { uint4 Value; };
VK_BINDING(0, 0) ConstantBuffer<Data> Count : register(b0);
VK_BINDING(1, 0) RWByteAddressBuffer Arguments : register(u0);
VK_BINDING(2, 0) RWStructuredBuffer<uint> Counts : register(u1);
[shader("compute")] [numthreads(1, 1, 1)] void CSMain() {
    Arguments.Store4(0, uint4(3, Count.Value.x, 0, 0));
    Arguments.Store4(16, uint4(3, Count.Value.x, 0, 0)); Arguments.Store(32, 0);
    Arguments.Store3(36, uint3(Count.Value.x, 1, 1));
    Counts[0] = Counts[1] = Counts[2] = 0;
}
)hlsl");
    auto raster = test::CompileFoundationGraphics(device, R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) RWStructuredBuffer<uint> Counts : register(u0);
[shader("vertex")] float4 VSMain(uint id : SV_VertexID) : SV_Position { return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1); }
[shader("pixel")] float4 PSMain(float4 p : SV_Position) : SV_Target0 { InterlockedAdd(Counts[(uint)p.x], 1); return 1; }
)hlsl");
    auto consumer = test::CompileFoundationCompute(device, R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) RWStructuredBuffer<uint> Counts : register(u0);
[shader("compute")] [numthreads(1, 1, 1)] void CSMain() { InterlockedAdd(Counts[2], 1); }
)hlsl");
    ASSERT_TRUE(producer);
    ASSERT_TRUE(raster);
    ASSERT_TRUE(consumer);
    const array<uint32_t, 3> indices{0, 1, 2};
    auto index = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
    ASSERT_TRUE(index);
    for (const uint32_t count : {0u, 1u, 17u}) {
        SCOPED_TRACE(count);
        Resources->BeginFlight(count + 2, Writes);
        auto graph = MakeGraph("indirect counts");
        const auto args = graph.CreateBuffer({48, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::Indirect, {}}, "arguments");
        auto counts = graph.CreateBuffer({12, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::CopySource, {}}, "counts");
        const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 2, 1, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "raster");
        struct Compute {
            ShaderProgram* Shader;
            render::ComputePipelineState* Program;
            PreparedShaderGroup Set;
            RgIndirectArgumentsHandle Indirect;
            RgBufferValue Arguments, Counts;
            array<uint32_t, 4> Count;
        };
        graph.AddComputePass<Compute>("generate",
            [&](Compute& data, RenderGraphComputeBuilder& builder) {
                data.Shader = producer.Get();
                data.Count = {count, 0, 0, 0};
                data.Arguments = builder.WriteBuffer(args, RgBufferAccess::UnorderedAccess, {0, 48});
                data.Counts = builder.WriteBuffer(counts, RgBufferAccess::UnorderedAccess, {0, 12}); },
            +[](Compute& data, RenderGraphPrepareContext& ctx) {
                const RgParameterBinding bindings[]{{"Count", 0, RgCBufferParameterBinding{std::as_bytes(std::span{data.Count})}},
                                                    {"Arguments", 0, RgBufferParameterBinding{data.Arguments, {0, 48}}},
                                                    {"Counts", 0, RgBufferParameterBinding{data.Counts, {0, 12}, 4}}};
                data.Program = ctx.ResolveComputePipeline(*data.Shader).Get();
                data.Set = ctx.CreateParameterSet(*data.Shader, 0, bindings);
                return data.Program != nullptr && data.Set.IsValid(); },
            +[](const Compute& data, RenderGraphComputeContext& pass) {
                pass.Encoder().BindComputePipelineState(data.Program);
                pass.Encoder().BindShaderParameterSet(data.Set);
                pass.Encoder().Dispatch(1, 1, 1); });
        struct Raster {
            ShaderProgram* Program;
            render::Buffer* Index;
            render::RenderBackend Backend;
            PreparedShaderGroup Set;
            RgIndirectArgumentsHandle Draw, Indexed;
            RgBufferValue Counts;
        };
        counts = graph.NextVersion(counts);
        graph.AddRasterPass<Raster>("count fragments",
            [&](Raster& data, RenderGraphRasterBuilder& builder) {
                data.Program = raster.Get(); data.Index = index.Get(); data.Backend = GetParam();
                builder.SetColorAttachment(0, color);
                data.Counts = builder.ReadWriteBuffer(counts, RgBufferAccess::UnorderedAccess, {0, 12});
                data.Draw = builder.ReadIndirectArguments(args, RgIndirectCommand::Draw, 0, 1);
                data.Indexed = builder.ReadIndirectArguments(args, RgIndirectCommand::DrawIndexed, 16, 1); },
            +[](Raster& data, RenderGraphPrepareContext& ctx) {
                const RgParameterBinding binding{"Counts", 0, RgBufferParameterBinding{data.Counts, {0, 12}, 4}};
                data.Set = ctx.CreateParameterSet(*data.Program, 0, std::span{&binding, 1});
                return data.Set.IsValid() && ctx.ValidateGeometryBuffer(data.Index, RgBufferAccess::Index); },
            +[](const Raster& data, RenderGraphRasterContext& pass) {
                MaterialPipelineState state; state.Primitive.Cull = render::CullMode::None;
                state.DepthStencil.DepthTestEnable = state.DepthStencil.DepthWriteEnable = false;
                const auto pso = data.Program->GetOrCreateGraphicsPipelineState(state, {}, PrimitiveTopology::TriangleList, pass.PassState());
                ASSERT_TRUE(pso);
                pass.Encoder().BindGraphicsPipelineState(pso.Get());
                pass.Encoder().BindShaderParameterSet(data.Set);
                pass.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 1, 1));
                pass.Encoder().SetScissor({0, 0, 1, 1});
                pass.Encoder().DrawIndirect(data.Draw);
                pass.Encoder().SetViewport(MakeViewport(data.Backend, 1, 0, 1, 1));
                pass.Encoder().SetScissor({1, 0, 1, 1});
                pass.Encoder().BindIndexBuffer({data.Index, 0, 4});
                pass.Encoder().DrawIndexedIndirect(data.Indexed); });
        counts = graph.NextVersion(counts);
        graph.AddComputePass<Compute>("indirect dispatch",
            [&](Compute& data, RenderGraphComputeBuilder& builder) {
                data.Shader = consumer.Get();
                data.Counts = builder.ReadWriteBuffer(counts, RgBufferAccess::UnorderedAccess, {0, 12});
                data.Indirect = builder.ReadIndirectArguments(args, RgIndirectCommand::Dispatch, 36, 1); },
            +[](Compute& data, RenderGraphPrepareContext& ctx) {
                const RgParameterBinding binding{"Counts", 0, RgBufferParameterBinding{data.Counts, {0, 12}, 4}};
                data.Program = ctx.ResolveComputePipeline(*data.Shader).Get();
                data.Set = ctx.CreateParameterSet(*data.Shader, 0, std::span{&binding, 1});
                return data.Program != nullptr && data.Set.IsValid(); },
            +[](const Compute& data, RenderGraphComputeContext& pass) {
                pass.Encoder().BindComputePipelineState(data.Program);
                pass.Encoder().BindShaderParameterSet(data.Set);
                pass.Encoder().DispatchIndirect(data.Indirect); });
        auto buffer = device.CreateBuffer({12, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
        ASSERT_TRUE(buffer);
        RenderExternalBuffer external{buffer.Get(), buffer->GetDesc(), render::BufferState::CopyDestination};
        const auto host = graph.NextVersion(graph.ImportBuffer(external, "host", RenderGraphExternalAccess::ObservableOutput));
        graph.AddCopyBufferPass("read counts", counts, host, 12);
        HostRead(graph, host);
        ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
        const auto bytes = Read(*buffer);
        array<uint32_t, 3> actual;
        std::memcpy(actual.data(), bytes.data(), sizeof(actual));
        for (const auto value : actual) EXPECT_EQ(value, count);
        EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndirect, 1u);
        EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexedIndirect, 1u);
        EXPECT_EQ(graph.GetReport().CommandCalls.IndirectDrawArguments, 2u);
        EXPECT_EQ(graph.GetReport().CommandCalls.Dispatch, 1u);
        EXPECT_EQ(graph.GetReport().CommandCalls.DispatchIndirect, 1u);
        EXPECT_EQ(graph.GetReport().CommandCalls.SetPipeline, 3u);
    }
}

TEST_P(GraphContractTest, GeneratedGeometryRequiresMatchingReadsAndPreparesGraphicsPsoBeforeRecording) {
    auto& device = *Context.Device;
    auto producer = test::CompileFoundationCompute(device, R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) RWStructuredBuffer<float3> Vertices : register(u0);
VK_BINDING(1, 0) RWStructuredBuffer<uint> Indices : register(u1);
VK_BINDING(2, 0) RWStructuredBuffer<uint> Arguments : register(u2);
[shader("compute")] [numthreads(1,1,1)] void CSMain() {
    Vertices[0] = float3(-1,-1,0); Vertices[1] = float3(3,-1,0); Vertices[2] = float3(-1,3,0);
    Indices[0] = 0; Indices[1] = 1; Indices[2] = 2;
    Arguments[0] = 3; Arguments[1] = 1; Arguments[2] = 0; Arguments[3] = 0; Arguments[4] = 0;
})hlsl");
    auto graphics = test::CompileFoundationGraphics(device, R"hlsl(
#include <core/platform.hlsli>
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position { return float4(p,1); }
[shader("pixel")] uint PSMain() : SV_Target0 { return 42; }
)hlsl");
    ASSERT_TRUE(producer);
    ASSERT_TRUE(graphics);
    PrimitiveVertexLayout layout;
    layout.Buffers = {{0, 12, render::VertexStepMode::Vertex}};
    layout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
    MaterialPipelineState state;
    state.Primitive.Cull = render::CullMode::None;
    state.DepthStencil.DepthTestEnable = state.DepthStencil.DepthWriteEnable = false;
    for (uint32_t scenario = 0; scenario < 5; ++scenario) {
        SCOPED_TRACE(scenario);
        Writes.Reset();
        Resources->BeginFlight(scenario + 2, Writes);
        auto graph = MakeGraph("generated native geometry");
        const auto vertices = graph.CreateBuffer({36, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::Vertex | render::BufferUse::Resource, {}}, "vertices");
        const auto indices = graph.CreateBuffer({12, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::Index | render::BufferUse::Resource, {}}, "indices");
        const auto arguments = graph.CreateBuffer({20, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::Indirect, {}}, "arguments");
        struct NativeGeometry {
            render::Buffer* Vertices{nullptr};
            render::Buffer* Indices{nullptr};
        } native;
        struct Compute {
            ShaderProgram* Shader;
            render::ComputePipelineState* Program;
            PreparedShaderGroup Set;
            RgBufferValue Vertices, Indices, Arguments;
            NativeGeometry* Native;
        };
        graph.AddComputePass<Compute>("generate",
            [&](Compute& data, RenderGraphComputeBuilder& builder) {
                data.Shader = producer.Get();
                data.Native = &native;
                data.Vertices = builder.WriteBuffer(vertices, RgBufferAccess::UnorderedAccess);
                data.Indices = builder.WriteBuffer(indices, RgBufferAccess::UnorderedAccess);
                data.Arguments = builder.WriteBuffer(arguments, RgBufferAccess::UnorderedAccess); },
            +[](Compute& data, RenderGraphPrepareContext& ctx) {
                const RgParameterBinding bindings[]{
                    {"Vertices", 0, RgBufferParameterBinding{data.Vertices, render::BufferRange::AllRange(), 12}},
                    {"Indices", 0, RgBufferParameterBinding{data.Indices, render::BufferRange::AllRange(), 4}},
                    {"Arguments", 0, RgBufferParameterBinding{data.Arguments, render::BufferRange::AllRange(), 4}}};
                data.Program = ctx.ResolveComputePipeline(*data.Shader).Get();
                data.Set = ctx.CreateParameterSet(*data.Shader, 0, bindings);
                data.Native->Vertices = ctx.GetBuffer(data.Vertices);
                data.Native->Indices = ctx.GetBuffer(data.Indices);
                return data.Program != nullptr && data.Set.IsValid(); },
            +[](const Compute& data, RenderGraphComputeContext& ctx) {
                ctx.Encoder().BindComputePipelineState(data.Program);
                ctx.Encoder().BindShaderParameterSet(data.Set);
                ctx.Encoder().Dispatch(1, 1, 1); });
        const auto target = graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::R32_UINT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, "target");
        struct Raster {
            render::GraphicsPipelineState* Program;
            RgIndirectArgumentsHandle Arguments;
            const NativeGeometry* Native;
            ShaderProgram* Shader;
            render::RenderBackend Backend;
            MaterialPipelineState State;
            PrimitiveVertexLayout Layout;
        };
        graph.AddRasterPass<Raster>("consume",
            [&](Raster& data, RenderGraphRasterBuilder& builder) {
                builder.SetColorAttachment(0, target);
                if (scenario != 0) builder.ReadBuffer(vertices, scenario == 1 ? RgBufferAccess::ShaderRead : RgBufferAccess::Vertex);
                builder.ReadBuffer(indices, scenario == 2 ? RgBufferAccess::ShaderRead : RgBufferAccess::Index);
                data.Arguments = builder.ReadIndirectArguments(arguments, RgIndirectCommand::DrawIndexed);
                data.Native = &native;
                data.Shader = graphics.Get();
                data.Backend = GetParam();
                data.State = state;
                data.Layout = layout; },
            +[](Raster& data, RenderGraphPrepareContext& ctx) {
                data.Program = ctx.ResolveGraphicsPipeline(*data.Shader, data.State, data.Layout).Get();
                if (!data.Program) return false;
                return ctx.ValidateGeometryBuffer(data.Native->Vertices, RgBufferAccess::Vertex) &&
                       ctx.ValidateGeometryBuffer(data.Native->Indices, RgBufferAccess::Index); },
            +[](const Raster& data, RenderGraphRasterContext& ctx) {
                // Native creation has already finished when the record callback starts, including the cold frame.
                const auto count = data.Shader->GetGraphicsPipelineStateCount();
                EXPECT_EQ(count, 1u);
                ctx.Encoder().BindGraphicsPipelineState(data.Program);
                const render::VertexBufferBinding binding{0, {data.Native->Vertices, 0, 36}};
                ctx.Encoder().BindVertexBuffers(std::span{&binding, 1});
                ctx.Encoder().BindIndexBuffer({data.Native->Indices, 0, 4});
                ctx.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 4, 4));
                ctx.Encoder().SetScissor({0, 0, 4, 4});
                ctx.Encoder().DrawIndexedIndirect(data.Arguments);
                EXPECT_EQ(data.Shader->GetGraphicsPipelineStateCount(), count); });
        const auto pitch = Align(uint64_t{16}, device.GetDetail().TextureDataPitchAlignment);
        auto readback = device.CreateBuffer({pitch * 4, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
        ASSERT_TRUE(readback);
        RenderExternalBuffer external{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
        const auto host = graph.NextVersion(graph.ImportBuffer(external, "readback", RenderGraphExternalAccess::ObservableOutput));
        graph.AddCopyTextureToBufferPass("read pixels", target, host);
        HostRead(graph, host);
        EXPECT_EQ(Run(graph), scenario >= 3) << graph.GetReport().ToText();
        EXPECT_EQ(graph.GetReport().GraphicsPipelinePreparations, 1u);
        EXPECT_EQ(graph.GetReport().GraphicsPipelineCreations, scenario == 0 ? 1u : 0u);
        if (scenario < 3) {
            ASSERT_FALSE(graph.GetReport().Diagnostics.empty());
            EXPECT_EQ(graph.GetReport().Diagnostics[0].Code, "UndeclaredGeometryRead");
        } else {
            const auto bytes = Read(*readback);
            uint32_t pixel = 0;
            std::memcpy(&pixel, bytes.data() + pitch + 4, sizeof(pixel));
            EXPECT_EQ(pixel, 42u);
            for (const auto [resource, after] : {std::pair{vertices.Index, render::BufferState::Vertex},
                                                 {indices.Index, render::BufferState::Index},
                                                 {arguments.Index, render::BufferState::Indirect}}) {
                EXPECT_TRUE(std::any_of(graph.GetReport().Barriers.begin(), graph.GetReport().Barriers.end(), [&](const auto& barrier) {
                    return barrier.Pass == 1 && barrier.Resource == resource && barrier.After == uint32_t(after);
                }));
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Backends, GraphContractTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
