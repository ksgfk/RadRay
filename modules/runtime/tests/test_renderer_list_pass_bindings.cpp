#include "foundation_graph_fixture.h"
#include <radray/runtime/render_framework/renderer_list_pass_bindings.h>

namespace radray {
namespace {

class RendererListPassBindingsTest : public test::FoundationGraphGpuTest {};

string BindingProgram(uint32_t group) {
    return fmt::format(R"hlsl(
#include <core/platform.hlsli>
struct Values {{ float4 Value; }};
VK_BINDING(0, 0) ConstantBuffer<Values> Native : register(b0, space0);
VK_BINDING(0, {0}) ConstantBuffer<Values> Graph : register(b0, space{0});
VK_BINDING(1, {0}) Texture2D<uint> Source : register(t0, space{0});
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position {{ return float4(p, 1); }}
[shader("pixel")] float4 PSMain() : SV_Target0 {{
    float v = (Native.Value.x + Graph.Value.x + Source.Load(int3(0, 0, 0))) / 255.0;
    return float4(v, v, v, 1);
}}
)hlsl",
                       group);
}

TEST_P(RendererListPassBindingsTest, B01B05B06AlternatingProgramsOwnConstantsAndDynamicOffsets) {
    auto& device = *Context.Device;
    render::ShaderProgramLayoutRecipe recipe;
    const render::ShaderLayoutSelector selector{.DeclarationName = "Native", .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
    recipe.D3D12.BufferPlacements.push_back({.Selector = selector, .Placement = render::D3D12BufferPlacement::RootDescriptor});
    recipe.Vulkan.BufferDescriptors.push_back({.Selector = selector, .Placement = render::VulkanBufferDescriptorPlacement::Dynamic});
    auto a = test::CompileFoundationGraphics(device, BindingProgram(3), recipe);
    auto b = test::CompileFoundationGraphics(device, BindingProgram(2), recipe);
    auto producer = test::CompileFoundationCompute(device, R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) RWTexture2D<uint> Destination : register(u0);
[shader("compute")] [numthreads(1, 1, 1)] void CSMain() { Destination[uint2(0, 0)] = 3; }
)hlsl");
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    ASSERT_TRUE(producer);
    const uint32_t alignment = static_cast<uint32_t>(std::max<uint64_t>(device.GetDetail().CBufferAlignment, 256));
    vector<byte> constants(alignment * 2);
    const float seven = 7, nineteen = 19;
    std::memcpy(constants.data(), &seven, sizeof(float));
    std::memcpy(constants.data() + alignment, &nineteen, sizeof(float));
    auto nativeBuffer = render::test::MakeUploadBuffer(device, constants, render::BufferUse::CBuffer);
    ASSERT_TRUE(nativeBuffer);
    vector<unique_ptr<render::ShaderParameterSet>> nativeSets;
    for (auto* program : {a.Get(), b.Get()}) {
        auto set = device.CreateShaderParameterSet({program->GetPipelineLayout(), 0});
        ASSERT_TRUE(set);
        ASSERT_TRUE(set->Set(program->GetPipelineLayout()->FindBinding("Native"), 0, render::ShaderBufferBinding{nativeBuffer.Get(), {0, 16}, 0}));
        ASSERT_TRUE(set->FlushWrites());
        nativeSets.push_back(set.Release());
    }
    vector<float> positions;
    vector<uint32_t> indices;
    for (uint32_t strip = 0; strip < 3; ++strip) {
        const float left = -1.0f + 2.0f * strip / 3.0f, right = -1.0f + 2.0f * (strip + 1) / 3.0f;
        positions.insert(positions.end(), {left, -1, .5f, right, -1, .5f, right, 1, .5f, left, 1, .5f});
        for (const uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u}) indices.push_back(strip * 4 + index);
    }
    auto vertices = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{positions}), render::BufferUse::Vertex);
    auto indexBuffer = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
    ASSERT_TRUE(vertices);
    ASSERT_TRUE(indexBuffer);
    GpuMesh::DrawData geometry;
    geometry.VertexBuffers = {{0, {vertices.Get(), 0, positions.size() * sizeof(float)}}};
    geometry.Ibv = {indexBuffer.Get(), 0, 4};
    geometry.VertexLayout.Buffers = {{0, 12, render::VertexStepMode::Vertex}};
    geometry.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
    RendererList list;
    for (uint32_t i = 0; i < 3; ++i) {
        auto* program = i == 1 ? b.Get() : a.Get();
        MeshDrawCommand draw;
        draw.Program = program;
        draw.Geometry = &geometry;
        draw.FirstIndex = i * 6;
        draw.IndexCount = 6;
        draw.PipelineState.Primitive.Cull = render::CullMode::None;
        draw.PipelineState.DepthStencil.DepthTestEnable = draw.PipelineState.DepthStencil.DepthWriteEnable = false;
        draw.Groups.push_back({0, nativeSets[i == 1 ? 1 : 0].get(), {{program->GetPipelineLayout()->FindBinding("Native"), i == 1 ? alignment : 0}}});
        ASSERT_TRUE(FinalizeMeshDrawCommand(draw));
        list.Commands.push_back(std::move(draw));
    }
    auto graph = MakeGraph("binding alternation");
    const auto source = graph.CreateTexture({render::TextureDimension::Dim2D, 1, 1, 1, 1, 1, render::TextureFormat::R32_UINT, render::MemoryType::Device, render::TextureUse::Resource | render::TextureUse::UnorderedAccess, {}}, "compute texture");
    struct Compute {
        RgComputeProgramHandle Program;
        RgParameterSetHandle Set;
    };
    graph.AddComputePass<Compute>("producer", [&](Compute& data, RenderGraphComputeBuilder& builder) {
        data.Program = builder.UseComputeProgram(*producer);
        const RgParameterBinding binding{"Destination", 0, RgTextureParameterBinding{source, {}, RgParameterAccess::Write}};
        data.Set = builder.CreateParameterSet(*producer, 0, std::span{&binding, 1}); }, +[](const Compute& data, RenderGraphComputeContext& context) {
        context.BindComputeProgram(data.Program); context.BindParameterSet(data.Set); context.Encoder().Dispatch(1, 1, 1); });
    const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 96, 32, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, "color");
    DrawExecutionStats stats;
    struct Raster {
        const RendererList* List;
        DrawExecutionStats* Stats;
        render::RenderBackend Backend;
        std::optional<RendererListPassBindings> Bindings;
        std::optional<PreparedRendererList> Prepared;
    };
    graph.AddRasterPass<Raster>("A B A", [&](Raster& data, RenderGraphRasterBuilder& builder) {
        data.List = &list; data.Stats = &stats; data.Backend = GetParam();
        builder.SetColorAttachment(0, color);
        array<float, 4> first{11, 0, 0, 0}, second{22, 0, 0, 0};
        const RgParameterBinding ap[]{{"Graph", 0, RgCBufferParameterBinding{std::as_bytes(std::span{first})}}, {"Source", 0, RgTextureParameterBinding{source}}};
        const RgParameterBinding bp[]{{"Graph", 0, RgCBufferParameterBinding{std::as_bytes(std::span{second})}}, {"Source", 0, RgTextureParameterBinding{source}}};
        const RendererListProgramParameters parameters[]{{a.Get(), 3, ap}, {b.Get(), 2, bp}};
        data.Bindings = RendererListPassBindings::Create(builder, list, parameters);
        if (data.Bindings) data.Prepared = PrepareRendererList(list, builder, &*data.Bindings);
        first.fill(199); second.fill(199);
        ASSERT_TRUE(data.Bindings); }, +[](const Raster& data, RenderGraphRasterContext& context) {
        context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 96, 32));
        context.Encoder().SetScissor({0, 0, 96, 32});
        SubmitRendererList(*data.Prepared, context, *data.Stats); });
    const uint64_t pitch = Align(uint64_t{96 * 4}, device.GetDetail().TextureDataPitchAlignment);
    auto readback = device.CreateBuffer({pitch * 32, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
    ASSERT_TRUE(readback);
    RenderExternalBuffer external{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    const auto host = graph.NextVersion(graph.ImportBuffer(external, "readback", RenderGraphExternalAccess::ObservableOutput));
    graph.AddCopyTextureToBufferPass("read color", color, host);
    HostRead(graph, host);
    ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
    EXPECT_TRUE(stats.Succeeded());
    EXPECT_EQ(stats.Draws, 3u);
    const auto bytes = Read(*readback);
    ASSERT_EQ(bytes.size(), pitch * 32);
    for (uint32_t i = 0; i < 3; ++i) {
        const auto value = std::to_integer<int>(bytes[pitch * 16 + (16 + 32 * i) * 4]);
        EXPECT_NEAR(value, i == 1 ? 44 : 21, 1);
    }
}

TEST_P(RendererListPassBindingsTest, B02B03B04RejectCollisionsMissingGroupsAndForeignScopeBeforeExecution) {
    auto& device = *Context.Device;
    auto program = test::CompileFoundationGraphics(device, R"hlsl(
#include <core/platform.hlsli>
struct Values { float4 Value; };
VK_BINDING(0, 0) ConstantBuffer<Values> ValuesBuffer : register(b0);
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position { return float4(p, 1); }
[shader("pixel")] float4 PSMain() : SV_Target0 { return ValuesBuffer.Value; }
)hlsl");
    ASSERT_TRUE(program);
    auto native = device.CreateShaderParameterSet({program->GetPipelineLayout(), 0});
    ASSERT_TRUE(native);
    for (uint32_t scenario = 0; scenario < 5; ++scenario) {
        RendererList list;
        MeshDrawCommand draw;
        draw.Program = program.Get();
        if (scenario == 0) draw.Groups.push_back({0, native.Get(), {}});
        if (scenario == 2) {
            // Populate the program requirements with a valid draw before a later draw omits its group.
            MeshDrawCommand valid;
            valid.Program = program.Get();
            valid.Groups.push_back({0, native.Get(), {}});
            list.Commands.push_back(std::move(valid));
        }
        list.Commands.push_back(draw);
        auto graph = MakeGraph("invalid bindings");
        auto other = MakeGraph("other graph");
        RgParameterSetHandle foreign;
        array<float, 4> bytes{1, 2, 3, 4};
        const RgParameterBinding parameter{"ValuesBuffer", 0, RgCBufferParameterBinding{std::as_bytes(std::span{bytes})}};
        if (scenario >= 3) {
            auto& owner = scenario == 3 ? graph : other;
            owner.AddRasterPass<test::EmptyGraphPass>("owner", [&](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { foreign = builder.CreateParameterSet(*program, 0, std::span{&parameter, 1}); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
        }
        bool called = false;
        struct Data {
            bool* Called;
        };
        graph.AddRasterPass<Data>("reject", [&](Data& data, RenderGraphRasterBuilder& builder) {
            data.Called = &called;
            const auto set = scenario >= 3 ? foreign : builder.CreateParameterSet(*program, 0, std::span{&parameter, 1});
            vector<RendererListPassBinding> bindings{{program.Get(), 0, set}};
            if (scenario == 1) bindings.push_back(bindings.front());
            if (scenario == 2) bindings.clear();
            EXPECT_FALSE(RendererListPassBindings::Build(builder, list, bindings));
            builder.SetSideEffect(); }, +[](const Data& data, RenderGraphRasterContext&) { *data.Called = true; });
        EXPECT_FALSE(Run(graph));
        EXPECT_FALSE(called);
        EXPECT_EQ(graph.GetReport().PhysicalAllocations, 0u);
        ASSERT_FALSE(graph.GetReport().Diagnostics.empty());
        if (scenario == 2) EXPECT_EQ(graph.GetReport().Diagnostics.front().Code, "RendererListMissingGroup");
        EXPECT_FALSE(graph.GetReport().Diagnostics.front().Pass.empty());
        EXPECT_FALSE(graph.GetReport().Diagnostics.front().Binding.empty());
    }
}

TEST_P(RendererListPassBindingsTest, InvalidItemIndexDuplicateAndIncompleteOrderFailBeforePreparation) {
    for (uint32_t scenario = 0; scenario < 3; ++scenario) {
        SCOPED_TRACE(scenario);
        RendererList list;
        list.Commands.resize(2);
        list.Items = {{{}, 0}, {{}, scenario == 0 ? 2u : 0u}};
        if (scenario == 2) list.Items.pop_back();
        auto graph = MakeGraph("invalid draw order");
        graph.AddRasterPass<test::EmptyGraphPass>("reject order", [&](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) {
            EXPECT_FALSE(PrepareRendererList(list, builder));
            builder.SetSideEffect();
        }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {
            ADD_FAILURE() << "An invalid draw order reached recording";
        });
        EXPECT_FALSE(Run(graph));
        EXPECT_EQ(graph.GetReport().GraphicsPipelineRequests, 0u);
        EXPECT_EQ(graph.GetReport().PhysicalAllocations, 0u);
        ASSERT_FALSE(graph.GetReport().Diagnostics.empty());
        EXPECT_EQ(graph.GetReport().Diagnostics.front().Code, "RendererListPreparation");
    }
}

TEST_P(RendererListPassBindingsTest, ProgramRecipesOwnResolvedGroupsAcrossLayoutChanges) {
    auto& device = *Context.Device;
    constexpr std::string_view source = R"hlsl(
#include <core/platform.hlsli>
struct Values { float4 Value; };
VK_BINDING(4, 2) ConstantBuffer<Values> A : register(b4, space2);
VK_BINDING(0, 2) ConstantBuffer<Values> B : register(b0, space2);
VK_BINDING(5, 2) Texture2D<float4> Images[2] : register(t0, space2);
VK_BINDING(8, 2) SamplerState LinearSampler : register(s0, space2);
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position { return float4(p, 1); }
[shader("pixel")] float4 PSMain() : SV_Target0 {
    return A.Value + B.Value + Images[0].SampleLevel(LinearSampler, float2(0, 0), 0) + Images[1].SampleLevel(LinearSampler, float2(0, 0), 0);
}
)hlsl";
    render::ShaderProgramLayoutRecipe dynamic;
    const render::ShaderLayoutSelector selector{.DeclarationName = "A", .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
    dynamic.D3D12.BufferPlacements.push_back({.Selector = selector, .Placement = render::D3D12BufferPlacement::RootDescriptor});
    dynamic.Vulkan.BufferDescriptors.push_back({.Selector = selector, .Placement = render::VulkanBufferDescriptorPlacement::Dynamic});
    auto a = test::CompileFoundationGraphics(device, source, dynamic);
    auto b = test::CompileFoundationGraphics(device, source);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(a->GetParameterGroupRecipeCount(), 0u);
    EXPECT_EQ(b->GetParameterGroupRecipeCount(), 0u);
    const auto& first = a->GetOrCreateParameterGroupRecipe(2);
    const auto& second = b->GetOrCreateParameterGroupRecipe(2);
    EXPECT_NE(&first, &second);
    ASSERT_EQ(first.Buffers.size(), 2u);
    EXPECT_EQ(first.TextureCount, 2u);
    EXPECT_EQ(first.SamplerCount, 1u);
    EXPECT_EQ(first.Textures.size(), 1u);
    EXPECT_EQ(first.Samplers.size(), 1u);
    const auto buffers = a->GetParameterLayout().Buffers();
    EXPECT_LT(buffers[first.Buffers[0].Index].BindingNumber, buffers[first.Buffers[1].Index].BindingNumber);
    for (const auto& buffer : first.Buffers) EXPECT_EQ(buffer.Dynamic, buffers[buffer.Index].Name == "A");
    for (const auto& buffer : second.Buffers) EXPECT_FALSE(buffer.Dynamic);
    // Populating other groups cannot invalidate a borrowed immutable recipe.
    for (uint32_t group = 10; group < 40; ++group) a->GetOrCreateParameterGroupRecipe(group);
    EXPECT_EQ(&a->GetOrCreateParameterGroupRecipe(2), &first);
    EXPECT_EQ(a->GetOrCreateParameterGroupRecipe(2).TextureCount, 2u);
    a = nullptr;
    auto replacement = test::CompileFoundationGraphics(device, source);
    ASSERT_TRUE(replacement);
    EXPECT_EQ(replacement->GetParameterGroupRecipeCount(), 0u);
    for (const auto& buffer : replacement->GetOrCreateParameterGroupRecipe(2).Buffers) EXPECT_FALSE(buffer.Dynamic);
}

TEST_P(RendererListPassBindingsTest, ExecutionItemsPreserveDynamicBindingsOrderAndRejectForeignGraph) {
    auto& device = *Context.Device;
    render::ShaderProgramLayoutRecipe recipe;
    const render::ShaderLayoutSelector selector{.DeclarationName = "Color", .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
    recipe.D3D12.BufferPlacements.push_back({.Selector = selector, .Placement = render::D3D12BufferPlacement::RootDescriptor});
    recipe.Vulkan.BufferDescriptors.push_back({.Selector = selector, .Placement = render::VulkanBufferDescriptorPlacement::Dynamic});
    auto program = test::CompileFoundationGraphics(device, R"hlsl(
#include <core/platform.hlsli>
struct Values { float4 Value; };
VK_BINDING(0, 0) ConstantBuffer<Values> Color : register(b0);
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position { return float4(p, 1); }
[shader("pixel")] float4 PSMain() : SV_Target0 { return Color.Value; }
)hlsl", recipe);
    ASSERT_TRUE(program);
    const uint32_t alignment = static_cast<uint32_t>(std::max<uint64_t>(device.GetDetail().CBufferAlignment, 256));
    vector<byte> constants(alignment * 3);
    const array<array<float, 4>, 3> colors{{{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}}};
    for (uint32_t index = 0; index < colors.size(); ++index)
        std::memcpy(constants.data() + index * alignment, colors[index].data(), sizeof(colors[index]));
    auto buffer = render::test::MakeUploadBuffer(device, constants, render::BufferUse::CBuffer);
    ASSERT_TRUE(buffer);
    const auto binding = program->GetPipelineLayout()->FindBinding("Color");
    auto set = device.CreateShaderParameterSet({program->GetPipelineLayout(), 0});
    ASSERT_TRUE(set);
    ASSERT_TRUE(set->Set(binding, 0, render::ShaderBufferBinding{buffer.Get(), {0, 16}, 0}));
    ASSERT_TRUE(set->FlushWrites());
    const array<float, 9> positions{-1, -1, .5f, 3, -1, .5f, -1, 3, .5f};
    const array<uint32_t, 3> indices{0, 1, 2};
    auto vertices = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{positions}), render::BufferUse::Vertex);
    auto indexBuffer = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
    ASSERT_TRUE(vertices);
    ASSERT_TRUE(indexBuffer);
    GpuMesh::DrawData geometry;
    geometry.VertexBuffers = {{0, {vertices.Get(), 0, sizeof(positions)}}};
    geometry.Ibv = {indexBuffer.Get(), 0, 4};
    geometry.VertexLayout.Buffers = {{0, 12, render::VertexStepMode::Vertex}};
    geometry.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
    RendererList list;
    for (uint32_t index = 0; index < colors.size(); ++index) {
        MeshDrawCommand draw;
        draw.Program = program.Get();
        draw.Geometry = &geometry;
        draw.IndexCount = 3;
        draw.PipelineState.Primitive.Cull = render::CullMode::None;
        draw.PipelineState.DepthStencil.DepthTestEnable = draw.PipelineState.DepthStencil.DepthWriteEnable = false;
        draw.Groups.push_back({0, set.Get(), {{binding, index * alignment}}});
        ASSERT_TRUE(FinalizeMeshDrawCommand(draw));
        list.Commands.push_back(std::move(draw));
    }
    list.Items = {{{}, 2}, {{}, 0}, {{}, 1}};
    auto graph = MakeGraph("ordered execution");
    const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, "color");
    DrawExecutionStats stats;
    std::optional<PreparedRendererList> prepared;
    struct Data {
        const PreparedRendererList* List;
        DrawExecutionStats* Stats;
        render::RenderBackend Backend;
    };
    graph.AddRasterPass<Data>("blue red green", [&](Data& data, RenderGraphRasterBuilder& builder) {
        builder.SetColorAttachment(0, color);
        prepared = PrepareRendererList(list, builder);
        ASSERT_TRUE(prepared);
        ASSERT_EQ(prepared->Draws.size(), 3u);
        for (size_t index = 0; index < prepared->Draws.size(); ++index) {
            EXPECT_EQ(prepared->Draws[index].Description, &list.GetCommand(index));
            EXPECT_EQ(prepared->Draws[index].Groups.data(), list.GetCommand(index).Groups.data());
        }
        data = {&*prepared, &stats, GetParam()};
    }, +[](const Data& data, RenderGraphRasterContext& context) {
        context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 16, 16));
        context.Encoder().SetScissor({0, 0, 16, 16});
        SubmitRendererList(*data.List, context, *data.Stats);
    });
    const auto pitch = Align(uint64_t{16 * 4}, device.GetDetail().TextureDataPitchAlignment);
    auto readback = device.CreateBuffer({pitch * 16, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
    ASSERT_TRUE(readback);
    RenderExternalBuffer external{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    const auto host = graph.NextVersion(graph.ImportBuffer(external, "readback", RenderGraphExternalAccess::ObservableOutput));
    graph.AddCopyTextureToBufferPass("read final color", color, host);
    HostRead(graph, host);
    ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
    EXPECT_EQ(stats.Draws, 3u);
    const auto bytes = Read(*readback);
    ASSERT_EQ(bytes.size(), pitch * 16);
    const size_t pixel = pitch * 8 + 8 * 4;
    EXPECT_EQ(std::to_integer<uint8_t>(bytes[pixel]), 0u);
    EXPECT_EQ(std::to_integer<uint8_t>(bytes[pixel + 1]), 255u);
    EXPECT_EQ(std::to_integer<uint8_t>(bytes[pixel + 2]), 0u);
    auto foreign = MakeGraph("foreign execution");
    const auto foreignColor = foreign.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "foreign color");
    DrawExecutionStats rejected;
    foreign.AddRasterPass<Data>("reject previous graph", [&](Data& data, RenderGraphRasterBuilder& builder) {
        data = {&*prepared, &rejected, GetParam()};
        builder.SetColorAttachment(0, foreignColor);
        builder.SetSideEffect();
    }, +[](const Data& data, RenderGraphRasterContext& context) {
        SubmitRendererList(*data.List, context, *data.Stats);
    });
    EXPECT_FALSE(Run(foreign));
    EXPECT_EQ(rejected.Draws, 0u);
    EXPECT_EQ(rejected.BindingFailure, 3u);
}

TEST_P(RendererListPassBindingsTest, B03TextureArraysRejectHolesKindsAndProgramsAndReadBothElements) {
    auto& device = *Context.Device;
    constexpr std::string_view source = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) Texture2D<float> Images[2] : register(t0);
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position { return float4(p, 1); }
[shader("pixel")] float PSMain() : SV_Target0 { return Images[0].Load(int3(0, 0, 0)) + Images[1].Load(int3(0, 0, 0)); }
)hlsl";
    auto program = test::CompileFoundationGraphics(device, source), other = test::CompileFoundationGraphics(device, source);
    ASSERT_TRUE(program);
    ASSERT_TRUE(other);
    const array<float, 9> positions{-1, -1, .5f, 3, -1, .5f, -1, 3, .5f};
    const array<uint32_t, 3> indices{0, 1, 2};
    auto vertices = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{positions}), render::BufferUse::Vertex);
    auto index = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
    ASSERT_TRUE(vertices);
    ASSERT_TRUE(index);
    GpuMesh::DrawData geometry;
    geometry.VertexBuffers = {{0, {vertices.Get(), 0, sizeof(positions)}}};
    geometry.Ibv = {index.Get(), 0, 4};
    geometry.VertexLayout.Buffers = {{0, 12, render::VertexStepMode::Vertex}};
    geometry.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
    RendererList list;
    MeshDrawCommand draw;
    draw.Program = program.Get();
    draw.Geometry = &geometry;
    draw.IndexCount = 3;
    draw.PipelineState.Primitive.Cull = render::CullMode::None;
    draw.PipelineState.DepthStencil.DepthTestEnable = draw.PipelineState.DepthStencil.DepthWriteEnable = false;
    ASSERT_TRUE(FinalizeMeshDrawCommand(draw));
    list.Commands.push_back(draw);
    for (uint32_t scenario = 0; scenario < 5; ++scenario) {
        SCOPED_TRACE(scenario);
        auto graph = MakeGraph("array declarations");
        array<RgTextureValue, 3> images;
        for (uint32_t i = 0; i < 3; ++i) {
            images[i] = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::R32_FLOAT, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource, {}}, fmt::format("image {}", i));
            if (i < 2) graph.AddRasterPass<test::EmptyGraphPass>("clear input", [=](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, images[i], {.Clear = {float(i + 1), 0, 0, 0}}); }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
        }
        DrawExecutionStats stats;
        struct Data {
            const RendererList* List;
            DrawExecutionStats* Stats;
            render::RenderBackend Backend;
            std::optional<RendererListPassBindings> Bindings;
            std::optional<PreparedRendererList> Prepared;
        };
        graph.AddRasterPass<Data>("array consumer", [&](Data& data, RenderGraphRasterBuilder& builder) {
            data.List = &list; data.Stats = &stats; data.Backend = GetParam(); builder.SetColorAttachment(0, images[2]);
            vector<RgParameterBinding> bindings{{"Images", 0, RgTextureParameterBinding{images[0]}}};
            if (scenario != 0) bindings.push_back({"Images", 1, RgTextureParameterBinding{images[1]}});
            if (scenario == 1) bindings.clear();
            const array<uint32_t, 4> wrong{1, 2, 3, 4};
            if (scenario == 2) bindings[1].Value = RgCBufferParameterBinding{std::as_bytes(std::span{wrong})};
            const auto set = builder.CreateParameterSet(scenario == 3 ? *other : *program, 0, bindings);
            const RendererListPassBinding parameters{program.Get(), 0, set};
            data.Bindings = RendererListPassBindings::Build(builder, list, std::span{&parameters, 1});
            if (data.Bindings) data.Prepared = PrepareRendererList(list, builder, &*data.Bindings); }, +[](const Data& data, RenderGraphRasterContext& context) {
            ASSERT_TRUE(data.Bindings); context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 16, 16)); context.Encoder().SetScissor({0, 0, 16, 16});
            SubmitRendererList(*data.Prepared, context, *data.Stats); });
        const auto pitch = Align(uint64_t{16 * 4}, device.GetDetail().TextureDataPitchAlignment);
        auto readback = device.CreateBuffer({pitch * 16, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
        ASSERT_TRUE(readback);
        RenderExternalBuffer external{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
        const auto host = graph.NextVersion(graph.ImportBuffer(external, "readback", RenderGraphExternalAccess::ObservableOutput));
        graph.AddCopyTextureToBufferPass("copy", images[2], host);
        HostRead(graph, host);
        if (scenario < 4) {
            EXPECT_FALSE(Run(graph));
            EXPECT_EQ(graph.GetReport().PhysicalAllocations, 0u);
            EXPECT_EQ(stats.Commands, 0u);
            ASSERT_FALSE(graph.GetReport().Diagnostics.empty());
            EXPECT_FALSE(graph.GetReport().Diagnostics.front().Binding.empty());
        } else {
            ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
            EXPECT_EQ(stats.Draws, 1u);
            EXPECT_TRUE(stats.Succeeded());
            const auto bytes = Read(*readback);
            float value;
            std::memcpy(&value, bytes.data(), 4);
            EXPECT_FLOAT_EQ(value, 3);
        }
    }
}

TEST_P(RendererListPassBindingsTest, B04RuntimeScopeViolationsAbortOnlyTheIsolatedProcessAndNextGraphWorks) {
    auto program = test::CompileFoundationCompute(*Context.Device, R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) RWStructuredBuffer<uint> Output : register(u0);
[shader("compute")] [numthreads(1, 1, 1)] void CSMain() { Output[0] = 17; }
)hlsl");
    ASSERT_TRUE(program);
    const auto runScope = [&](uint32_t mode) {
        auto graph = MakeGraph("scope execution");
        auto owner = MakeGraph("foreign scope");
        RgParameterSetHandle first, foreign;
        struct Data {
            RgComputeProgramHandle Program;
            RgParameterSetHandle Set;
        };
        const auto append = [&](RenderGraph& target, RgParameterSetHandle& saved, bool misuse) {
            const auto output = target.CreateBuffer({4, render::MemoryType::Device, render::BufferUse::UnorderedAccess, {}}, "value");
            target.AddComputePass<Data>("bind scope", [&](Data& data, RenderGraphComputeBuilder& builder) {
                const RgParameterBinding binding{"Output", 0, RgBufferParameterBinding{output, {0, 4}, 4, render::TextureFormat::UNKNOWN, RgParameterAccess::Write}};
                data.Program = builder.UseComputeProgram(*program);
                saved = builder.CreateParameterSet(*program, 0, std::span{&binding, 1});
                data.Set = misuse ? (mode == 1 ? first : foreign) : saved;
                builder.SetSideEffect(); }, +[](const Data& data, RenderGraphComputeContext& pass) { pass.BindComputeProgram(data.Program); pass.BindParameterSet(data.Set); pass.Encoder().Dispatch(1, 1, 1); });
        };
        append(owner, foreign, false);
        append(graph, first, false);
        RgParameterSetHandle second;
        append(graph, second, mode != 0);
        return Run(graph);
    };
    for (uint32_t mode : {1u, 2u}) {
        EXPECT_DEATH({
            SetLogCallback(+[](LogLevel, std::string_view message, void*) { std::fwrite(message.data(), 1, message.size(), stderr); std::fflush(stderr); }, nullptr);
            runScope(mode); }, "RenderGraph parameter.set");
        ASSERT_TRUE(runScope(0));
    }
}

INSTANTIATE_TEST_SUITE_P(Backends, RendererListPassBindingsTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
