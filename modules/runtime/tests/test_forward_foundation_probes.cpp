#include "foundation_graph_fixture.h"
#include <fstream>
#include <radray/runtime/forward_pipeline/forward_pipeline.h>

namespace radray {
namespace {

class ForwardFoundationProbe : public test::FoundationGraphGpuTest {};

TEST_P(ForwardFoundationProbe, ForwardEffectShadersAndMaterialPassesHaveUsableArtifacts) {
    const auto read = [](std::string_view name) {
        std::ifstream stream{std::filesystem::path{RADRAY_PROJECT_DIR} / "shaderlib/pipelines/forward" / name};
        return string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    };
    vector<unique_ptr<ShaderProgram>> materialPrograms;
    const std::string_view names[]{"pbr.hlsl", "depth_normals_motion.hlsl", "shadow_caster.hlsl"};
    for (const auto name : names) {
        SCOPED_TRACE(name);
        auto program = test::CompileFoundationGraphics(*Context.Device, read(name), ForwardPipeline::GetLayoutRecipe());
        ASSERT_TRUE(program);
        materialPrograms.push_back(program.Release());
    }
    const auto technique = MaterialTechnique::Create({{"ForwardLit", materialPrograms[0].get(), "ForwardMaterial", {}},
                                                      {"DepthNormalsMotion", materialPrograms[1].get(), "ForwardMaterial", {}},
                                                      {"ShadowCaster", materialPrograms[2].get(), "ForwardMaterial", {}}},
                                                     "ForwardLit");
    ASSERT_TRUE(technique);
    const std::string_view effects[]{"linear_depth", "depth_pyramid", "ambient_occlusion", "ao_blur", "temporal_resolve", "bloom", "bloom", "bloom", "output", "tile_lights", "sky", "firefly_update", "firefly_draw", "debug"};
    for (uint32_t effect = 0; effect < 14; ++effect) {
        SCOPED_TRACE(effect);
        const auto source = fmt::format("#define FORWARD_EFFECT {}\n{}", effect, read(fmt::format("{}.hlsl", effects[effect])));
        auto program = test::CompileFoundationGraphics(*Context.Device, source);
        ASSERT_TRUE(program);
        if (effect != 8 && effect != 10 && effect != 12) EXPECT_TRUE(program->GetOrCreateComputePipelineState());
    }
}

TEST_P(ForwardFoundationProbe, R01DepthArrayIsSampledWithoutLayerAliasing) {
    auto& device = *Context.Device;
    const auto usage = render::TextureUse::DepthStencilWrite | render::TextureUse::Resource;
    ASSERT_TRUE(device.QueryTextureSupport({render::TextureDimension::Dim2DArray, render::TextureFormat::D32_FLOAT, usage}).Supported);
    auto shader = test::CompileFoundationCompute(device, R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) Texture2DArray<float> Depths : register(t0);
VK_BINDING(1, 0) RWStructuredBuffer<float> Values : register(u0);
[shader("compute")] [numthreads(4, 1, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
    Values[id.x] = Depths.Load(int4(0, 0, id.x, 0));
}
)hlsl");
    ASSERT_TRUE(shader);
    auto graph = MakeGraph("depth layers");
    const auto depth = graph.CreateTexture({render::TextureDimension::Dim2DArray, 16, 16, 4, 1, 1, render::TextureFormat::D32_FLOAT, render::MemoryType::Device, usage, {}}, "depth array");
    for (uint32_t layer = 0; layer < 4; ++layer) {
        graph.AddRasterPass<test::EmptyGraphPass>(fmt::format("layer {}", layer), [=](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) {
            RgDepthAttachmentDesc desc; desc.View.Range = {layer, 1, 0, 1}; desc.Clear = {.2f * (layer + 1), 0};
            builder.SetDepthAttachment(depth, desc); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
    }
    const auto values = graph.CreateBuffer({16, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::CopySource, {}}, "values");
    struct Data {
        RgComputeProgramHandle Program;
        RgParameterSetHandle Set;
    };
    graph.AddComputePass<Data>("sample layers", [&](Data& data, RenderGraphComputeBuilder& builder) {
        data.Program = builder.UseComputeProgram(*shader);
        const RgParameterBinding bindings[]{{"Depths", 0, RgTextureParameterBinding{depth}},
                                           {"Values", 0, RgBufferParameterBinding{values, {0, 16}, 4, render::TextureFormat::UNKNOWN, RgParameterAccess::Write}}};
        data.Set = builder.CreateParameterSet(*shader, 0, bindings); }, +[](const Data& data, RenderGraphComputeContext& context) {
        context.BindComputeProgram(data.Program); context.BindParameterSet(data.Set); context.Encoder().Dispatch(1, 1, 1); });
    auto readback = device.CreateBuffer({16, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
    ASSERT_TRUE(readback);
    RenderExternalBuffer external{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    const auto host = graph.ImportBuffer(external, "readback", RenderGraphExternalAccess::ObservableOutput);
    graph.AddCopyBufferPass("copy", values, host, 16);
    HostRead(graph, host);
    ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
    const auto bytes = Read(*readback);
    ASSERT_EQ(bytes.size(), 16u);
    for (uint32_t layer = 0; layer < 4; ++layer) {
        float value;
        std::memcpy(&value, bytes.data() + layer * 4, 4);
        EXPECT_NEAR(value, .2f * (layer + 1), 1e-5f);
    }
}

TEST_P(ForwardFoundationProbe, R02DepthReadOnlyAttachmentCanAlsoBeSampled) {
    auto& device = *Context.Device;
    auto shader = test::CompileFoundationGraphics(device, R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) Texture2D<float> Depth : register(t0);
[shader("vertex")] float4 VSMain(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
[shader("pixel")] float4 PSMain() : SV_Target0 { return Depth.Load(int3(0, 0, 0)).xxxx; }
)hlsl");
    ASSERT_TRUE(shader);
    auto graph = MakeGraph("read-only depth sample");
    const auto depth = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::D32_FLOAT, render::MemoryType::Device, render::TextureUse::DepthStencilRead | render::TextureUse::DepthStencilWrite | render::TextureUse::Resource, {}}, "depth");
    graph.AddRasterPass<test::EmptyGraphPass>("depth producer", [=](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { builder.SetDepthAttachment(depth, {.Clear = {.25f, 0}}); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
    const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, "sampled depth");
    struct Data {
        ShaderProgram* Program;
        render::RenderBackend Backend;
        RgParameterSetHandle Set;
        bool* PsoCreated;
    };
    bool created = false;
    graph.AddRasterPass<Data>("depth reader", [&](Data& data, RenderGraphRasterBuilder& builder) {
        data.Program = shader.Get(); data.Backend = GetParam(); data.PsoCreated = &created;
        builder.SetColorAttachment(0, color);
        builder.SetDepthAttachment(depth, {.Load = render::LoadAction::Load, .ReadOnly = true});
        const RgParameterBinding binding{"Depth", 0, RgTextureParameterBinding{depth}};
        data.Set = builder.CreateParameterSet(*shader, 0, std::span{&binding, 1}); }, +[](const Data& data, RenderGraphRasterContext& context) {
        MaterialPipelineState state;
        state.Primitive.Cull = render::CullMode::None; state.DepthStencil.DepthWriteEnable = true;
        EXPECT_FALSE(data.Program->GetOrCreateGraphicsPipelineState(state, {}, PrimitiveTopology::TriangleList, context.PassState()));
        state.DepthStencil.DepthWriteEnable = false;
        const auto pso = data.Program->GetOrCreateGraphicsPipelineState(state, {}, PrimitiveTopology::TriangleList, context.PassState());
        *data.PsoCreated = pso.HasValue();
        if (!pso) return;
        context.Encoder().BindGraphicsPipelineState(pso.Get()); context.BindParameterSet(data.Set);
        context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 16, 16)); context.Encoder().SetScissor({0, 0, 16, 16});
        context.Encoder().Draw(3, 1, 0, 0); });
    const uint64_t pitch = Align(uint64_t{16 * 4}, device.GetDetail().TextureDataPitchAlignment);
    auto readback = device.CreateBuffer({pitch * 16, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
    ASSERT_TRUE(readback);
    RenderExternalBuffer external{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    const auto host = graph.ImportBuffer(external, "readback", RenderGraphExternalAccess::ObservableOutput);
    graph.AddCopyTextureToBufferPass("copy", color, host);
    HostRead(graph, host);
    ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
    EXPECT_TRUE(created);
    const auto bytes = Read(*readback);
    ASSERT_EQ(bytes.size(), pitch * 16);
    float value;
    std::memcpy(&value, bytes.data() + pitch * 8 + 8 * 4, 4);
    EXPECT_FLOAT_EQ(value, .25f);
}

TEST_P(ForwardFoundationProbe, R03FloatingMrtFormatsAndSampleCountHaveDistinctPsoKeys) {
    auto& device = *Context.Device;
    auto shader = test::CompileFoundationGraphics(device, R"hlsl(
[shader("vertex")] float4 VSMain(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
struct Output { float4 A : SV_Target0; float2 B : SV_Target1; };
[shader("pixel")] Output PSMain() { Output o; o.A = float4(-2, .5, 4, 1); o.B = float2(.125, 3); return o; }
)hlsl");
    ASSERT_TRUE(shader);
    for (uint32_t scenario = 0; scenario < 3; ++scenario) {
        SCOPED_TRACE(scenario);
        const uint32_t samples = scenario == 2 ? 4 : 1;
        const render::TextureFormat formats[]{scenario == 1 ? render::TextureFormat::RGBA32_FLOAT : render::TextureFormat::RGBA16_FLOAT,
                                              scenario == 1 ? render::TextureFormat::RG16_FLOAT : render::TextureFormat::RG32_FLOAT};
        auto graph = MakeGraph("floating MRT");
        RgTextureHandle attachments[2], resolved[2];
        for (uint32_t i = 0; i < 2; ++i) {
            attachments[i] = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, samples, formats[i], render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, fmt::format("MRT {}", i));
            resolved[i] = attachments[i];
        }
        struct Data {
            ShaderProgram* Program;
            render::RenderBackend Backend;
            bool* Drawn;
        };
        bool drawn = false;
        graph.AddRasterPass<Data>("MRT", [&](Data& data, RenderGraphRasterBuilder& builder) {
            data = {shader.Get(), GetParam(), &drawn}; builder.SetColorAttachment(0, attachments[0]); builder.SetColorAttachment(1, attachments[1]); }, +[](const Data& data, RenderGraphRasterContext& context) {
            MaterialPipelineState state; state.Primitive.Cull = render::CullMode::None;
            state.DepthStencil.DepthTestEnable = state.DepthStencil.DepthWriteEnable = false;
            const auto pso = data.Program->GetOrCreateGraphicsPipelineState(state, {}, PrimitiveTopology::TriangleList, context.PassState());
            if (!pso) return;
            *data.Drawn = true; context.Encoder().BindGraphicsPipelineState(pso.Get());
            context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 16, 16)); context.Encoder().SetScissor({0, 0, 16, 16}); context.Encoder().Draw(3, 1, 0, 0); });
        array<unique_ptr<render::Buffer>, 2> readback;
        array<RenderExternalBuffer, 2> imports{};
        uint64_t pitches[2];
        for (uint32_t i = 0; i < 2; ++i) {
            if (samples > 1) {
                resolved[i] = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, formats[i], render::MemoryType::Device, render::TextureUse::CopySource | render::TextureUse::CopyDestination, {}}, "resolved MRT");
                graph.AddResolveTexturePass("resolve MRT", attachments[i], resolved[i]);
            }
            pitches[i] = Align(uint64_t{16} * render::GetTextureFormatBytesPerPixel(formats[i]), device.GetDetail().TextureDataPitchAlignment);
            auto buffer = device.CreateBuffer({pitches[i] * 16, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
            ASSERT_TRUE(buffer);
            readback[i] = buffer.Release();
            imports[i] = {readback[i].get(), readback[i]->GetDesc(), render::BufferState::CopyDestination};
            const auto host = graph.ImportBuffer(imports[i], "MRT readback", RenderGraphExternalAccess::ObservableOutput);
            graph.AddCopyTextureToBufferPass("copy MRT", resolved[i], host);
            HostRead(graph, host);
        }
        ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
        EXPECT_TRUE(drawn);
        EXPECT_EQ(shader->GetGraphicsPipelineStateCount(), scenario + 1);
        for (uint32_t i = 0; i < 2; ++i) {
            const auto bytes = Read(*readback[i]);
            ASSERT_FALSE(bytes.empty());
            const uint32_t count = i == 0 ? 4 : 2;
            const bool half = formats[i] == render::TextureFormat::RGBA16_FLOAT || formats[i] == render::TextureFormat::RG16_FLOAT;
            const uint64_t offset = pitches[i] * 8 + 8 * count * (half ? 2 : 4);
            const uint16_t expectedHalf[2][4]{{0xc000, 0x3800, 0x4400, 0x3c00}, {0x3000, 0x4200, 0, 0}};
            const float expectedFloat[2][4]{{-2, .5f, 4, 1}, {.125f, 3, 0, 0}};
            for (uint32_t c = 0; c < count; ++c) {
                if (half) {
                    uint16_t value;
                    std::memcpy(&value, bytes.data() + offset + c * 2, 2);
                    EXPECT_EQ(value, expectedHalf[i][c]);
                } else {
                    float value;
                    std::memcpy(&value, bytes.data() + offset + c * 4, 4);
                    EXPECT_FLOAT_EQ(value, expectedFloat[i][c]);
                }
            }
        }
    }
}

TEST_P(ForwardFoundationProbe, R08SrgbAttachmentEncodesOnceAndSamplingDecodesOnce) {
    auto& device = *Context.Device;
    auto shader = test::CompileFoundationGraphics(device, R"hlsl(
[shader("vertex")] float4 VSMain(uint id : SV_VertexID) : SV_Position { return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1); }
[shader("pixel")] float4 PSMain() : SV_Target0 { return float4(.21404114, .0031308, 1, 1); }
)hlsl");
    auto reader = test::CompileFoundationCompute(device, R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) Texture2D<float4> Source : register(t0);
VK_BINDING(1, 0) RWStructuredBuffer<float4> Result : register(u0);
[shader("compute")] [numthreads(1, 1, 1)] void CSMain() { Result[0] = Source.Load(int3(0, 0, 0)); }
)hlsl");
    ASSERT_TRUE(shader);
    ASSERT_TRUE(reader);
    auto graph = MakeGraph("sRGB transfer");
    const auto texture = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::RGBA8_UNORM_SRGB, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource | render::TextureUse::Resource, {}}, "sRGB attachment");
    struct Raster {
        ShaderProgram* Program;
        render::RenderBackend Backend;
        bool* Drawn;
    };
    bool drawn = false;
    graph.AddRasterPass<Raster>("linear write", [&](Raster& data, RenderGraphRasterBuilder& builder) {
        data = {shader.Get(), GetParam(), &drawn}; builder.SetColorAttachment(0, texture); }, +[](const Raster& data, RenderGraphRasterContext& context) {
        MaterialPipelineState state; state.Primitive.Cull = render::CullMode::None;
        state.DepthStencil.DepthTestEnable = state.DepthStencil.DepthWriteEnable = false;
        const auto pso = data.Program->GetOrCreateGraphicsPipelineState(state, {}, PrimitiveTopology::TriangleList, context.PassState());
        if (!pso) return;
        *data.Drawn = true; context.Encoder().BindGraphicsPipelineState(pso.Get()); context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 16, 16));
        context.Encoder().SetScissor({0, 0, 16, 16}); context.Encoder().Draw(3, 1, 0, 0); });
    const auto result = graph.CreateBuffer({16, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::CopySource, {}}, "decoded value");
    struct Compute {
        RgComputeProgramHandle Program;
        RgParameterSetHandle Set;
    };
    graph.AddComputePass<Compute>("decode sample", [&](Compute& data, RenderGraphComputeBuilder& builder) {
        data.Program = builder.UseComputeProgram(*reader);
        const RgParameterBinding bindings[]{{"Source", 0, RgTextureParameterBinding{texture}}, {"Result", 0, RgBufferParameterBinding{result, {0, 16}, 16, render::TextureFormat::UNKNOWN, RgParameterAccess::Write}}};
        data.Set = builder.CreateParameterSet(*reader, 0, bindings); }, +[](const Compute& data, RenderGraphComputeContext& context) { context.BindComputeProgram(data.Program); context.BindParameterSet(data.Set); context.Encoder().Dispatch(1, 1, 1); });
    const uint64_t pitch = Align(64, device.GetDetail().TextureDataPitchAlignment);
    auto encoded = device.CreateBuffer({pitch * 16, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
    auto decoded = device.CreateBuffer({16, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
    ASSERT_TRUE(encoded);
    ASSERT_TRUE(decoded);
    RenderExternalBuffer a{encoded.Get(), encoded->GetDesc(), render::BufferState::CopyDestination}, b{decoded.Get(), decoded->GetDesc(), render::BufferState::CopyDestination};
    const auto encodedHost = graph.ImportBuffer(a, "encoded", RenderGraphExternalAccess::ObservableOutput), decodedHost = graph.ImportBuffer(b, "decoded", RenderGraphExternalAccess::ObservableOutput);
    graph.AddCopyTextureToBufferPass("encoded bytes", texture, encodedHost);
    graph.AddCopyBufferPass("decoded floats", result, decodedHost, 16);
    HostRead(graph, encodedHost);
    HostRead(graph, decodedHost);
    ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
    EXPECT_TRUE(drawn);
    const auto encodedBytes = Read(*encoded);
    ASSERT_GE(encodedBytes.size(), 4u);
    EXPECT_NEAR(std::to_integer<int>(encodedBytes[0]), 128, 1);
    EXPECT_NEAR(std::to_integer<int>(encodedBytes[1]), 10, 1);
    EXPECT_EQ(encodedBytes[2], byte{255});
    EXPECT_EQ(encodedBytes[3], byte{255});
    const auto decodedBytes = Read(*decoded);
    ASSERT_EQ(decodedBytes.size(), 16u);
    float values[4];
    std::memcpy(values, decodedBytes.data(), 16);
    EXPECT_NEAR(values[0], .21404114f, .002f);
    EXPECT_NEAR(values[1], .0031308f, .0004f);
    EXPECT_FLOAT_EQ(values[2], 1);
    EXPECT_FLOAT_EQ(values[3], 1);
}

TEST_P(ForwardFoundationProbe, R07OddDimensionsRespectPitchPlacementAndSentinels) {
    auto& device = *Context.Device;
    auto shader = test::CompileFoundationGraphics(device, R"hlsl(
[shader("vertex")] float4 VSMain(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3 : -1, id == 1 ? 3 : -1, 0, 1);
}
[shader("pixel")] float4 PSMain(float4 p : SV_Position) : SV_Target0 {
    uint2 xy = (uint2)p.xy; return float4(xy.x, xy.y, xy.x + xy.y, 255) / 255.0;
}
)hlsl");
    ASSERT_TRUE(shader);
    const uint64_t pitch = Align(uint64_t{13 * 4}, device.GetDetail().TextureDataPitchAlignment);
    const uint64_t offset = Align(Align(uint64_t{31}, uint64_t{render::GetTextureFormatBytesPerPixel(render::TextureFormat::RGBA8_UNORM)}), device.GetDetail().TextureDataPlacementAlignment);
    vector<byte> initial(offset + pitch * 7 + 37, byte{0xcc});
    auto upload = render::test::MakeUploadBuffer(device, initial, render::BufferUse::CopySource);
    auto readback = device.CreateBuffer({initial.size(), render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
    ASSERT_TRUE(upload);
    ASSERT_TRUE(readback);
    RenderExternalBuffer uploadExternal{upload.Get(), upload->GetDesc(), render::BufferState::HostWrite, true};
    RenderExternalBuffer output{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    auto graph = MakeGraph("odd readback");
    const auto source = graph.ImportBuffer(uploadExternal, "sentinels", RenderGraphExternalAccess::ReadOnly);
    const auto host = graph.ImportBuffer(output, "readback", RenderGraphExternalAccess::ObservableOutput);
    graph.AddCopyBufferPass("initialize entire buffer", source, host, initial.size());
    const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 13, 7, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, "pattern");
    struct Data {
        ShaderProgram* Program;
        render::RenderBackend Backend;
        bool* Drawn;
    };
    bool drawn = false;
    graph.AddRasterPass<Data>("pattern", [&](Data& data, RenderGraphRasterBuilder& builder) {
        data = {shader.Get(), GetParam(), &drawn}; builder.SetColorAttachment(0, color); }, +[](const Data& data, RenderGraphRasterContext& context) {
        MaterialPipelineState state; state.Primitive.Cull = render::CullMode::None;
        state.DepthStencil.DepthTestEnable = state.DepthStencil.DepthWriteEnable = false;
        auto pso = data.Program->GetOrCreateGraphicsPipelineState(state, {}, PrimitiveTopology::TriangleList, context.PassState());
        if (!pso) return;
        *data.Drawn = true;
        context.Encoder().BindGraphicsPipelineState(pso.Get());
        context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 13, 7)); context.Encoder().SetScissor({0, 0, 13, 7});
        context.Encoder().Draw(3, 1, 0, 0); });
    graph.AddCopyTextureToBufferPass("partial readback", color, host, {0, 1, 0, 1}, offset);
    HostRead(graph, host);
    ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
    EXPECT_TRUE(drawn);
    const auto bytes = Read(*readback);
    ASSERT_EQ(bytes.size(), initial.size());
    for (uint64_t i = 0; i < offset; ++i) EXPECT_EQ(bytes[i], byte{0xcc});
    for (uint64_t i = offset + pitch * 7; i < bytes.size(); ++i) EXPECT_EQ(bytes[i], byte{0xcc});
    for (uint32_t y = 0; y < 7; ++y)
        for (uint32_t x = 0; x < 13; ++x) {
            const auto base = offset + pitch * y + 4 * x;
            EXPECT_EQ(std::to_integer<uint32_t>(bytes[base]), x);
            EXPECT_EQ(std::to_integer<uint32_t>(bytes[base + 1]), y);
            EXPECT_EQ(std::to_integer<uint32_t>(bytes[base + 2]), x + y);
            EXPECT_EQ(bytes[base + 3], byte{255});
        }
}

INSTANTIATE_TEST_SUITE_P(Backends, ForwardFoundationProbe, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
