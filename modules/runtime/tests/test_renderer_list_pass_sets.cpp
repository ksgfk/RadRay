#include "foundation_graph_fixture.h"
#include <radray/runtime/render_framework/renderer_list_pass_sets.h>

namespace radray {
namespace {

class RendererListPassSetsTest : public test::FoundationGraphGpuTest {};

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

constexpr std::string_view kSingleGroupProgram = R"hlsl(
#include <core/platform.hlsli>
struct Values { float4 Value; };
VK_BINDING(0, 0) ConstantBuffer<Values> ValuesBuffer : register(b0);
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position { return float4(p, 1); }
[shader("pixel")] float4 PSMain() : SV_Target0 { return ValuesBuffer.Value; }
)hlsl";

RgTextureValue MakeSmallTarget(RenderGraph& graph, std::string_view name) {
    return graph.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, name);
}

TEST_P(RendererListPassSetsTest, B01B05B06AlternatingProgramsOwnConstantsAndDynamicOffsets) {
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
        ShaderProgram* Program;
        RgTextureViewHandle Destination;
        render::ComputePipelineState* Pipeline;
        PreparedShaderGroup Set;
    };
    graph.AddComputePass<Compute>("producer", [&](Compute& data, RenderGraphComputeBuilder& builder) {
        data.Program = producer.Get();
        data.Destination = builder.WriteTexture(source); }, +[](Compute& data, RenderGraphPrepareContext& ctx) {
        const RgParameterBinding binding{"Destination", 0, RgTextureParameterBinding{data.Destination}};
        data.Set = ctx.CreateParameterSet(*data.Program, 0, std::span{&binding, 1});
        data.Pipeline = ctx.ResolveComputePipeline(*data.Program).Get();
        return data.Set.IsValid() && data.Pipeline != nullptr; }, +[](const Compute& data, RenderGraphComputeContext& context) {
        context.Encoder().BindComputePipelineState(data.Pipeline);
        context.Encoder().BindShaderParameterSet(data.Set);
        context.Encoder().Dispatch(1, 1, 1); });
    const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 96, 32, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, "color");
    DrawExecutionStats stats;
    struct Raster {
        const RendererList* List;
        DrawExecutionStats* Stats;
        render::RenderBackend Backend;
        ShaderProgram* A;
        ShaderProgram* B;
        RgTextureViewHandle Source;
        array<float, 4> First, Second;
        std::optional<RendererListPassSets> Sets;
        std::optional<PreparedRendererList> Prepared;
    };
    graph.AddRasterPass<Raster>("A B A", [&](Raster& data, RenderGraphRasterBuilder& builder) {
        data.List = &list; data.Stats = &stats; data.Backend = GetParam();
        data.A = a.Get(); data.B = b.Get();
        data.First = {11, 0, 0, 0};
        data.Second = {22, 0, 0, 0};
        builder.SetColorAttachment(0, color);
        data.Source = builder.ReadTexture(source); }, +[](Raster& data, RenderGraphPrepareContext& ctx) {
        const RgParameterBinding ap[]{{"Graph", 0, RgCBufferParameterBinding{std::as_bytes(std::span{data.First})}}, {"Source", 0, RgTextureParameterBinding{data.Source}}};
        const RgParameterBinding bp[]{{"Graph", 0, RgCBufferParameterBinding{std::as_bytes(std::span{data.Second})}}, {"Source", 0, RgTextureParameterBinding{data.Source}}};
        const RendererListProgramParameters parameters[]{{data.A, 3, ap}, {data.B, 2, bp}};
        data.Sets = RendererListPassSets::Create(ctx, *data.List, parameters);
        if (!data.Sets) return false;
        EXPECT_EQ(data.Sets->Find(*data.A).size(), 1u);
        EXPECT_EQ(data.Sets->Find(*data.B).size(), 1u);
        // The created sets own their constants, so overwriting the source values cannot change a draw.
        data.First.fill(199);
        data.Second.fill(199);
        data.Prepared = PrepareRendererList(*data.List, ctx, &*data.Sets);
        return data.Prepared.has_value(); }, +[](const Raster& data, RenderGraphRasterContext& context) {
        context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 96, 32));
        context.Encoder().SetScissor({0, 0, 96, 32});
        RecordRendererList(*data.Prepared, context, *data.Stats); });
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

TEST_P(RendererListPassSetsTest, B02B03PrepareRejectsCollisionsNativeGroupsMissingGroupsAndForeignPrograms) {
    auto& device = *Context.Device;
    auto program = test::CompileFoundationGraphics(device, kSingleGroupProgram);
    auto other = test::CompileFoundationGraphics(device, kSingleGroupProgram);
    ASSERT_TRUE(program);
    ASSERT_TRUE(other);
    auto native = device.CreateShaderParameterSet({program->GetPipelineLayout(), 0});
    ASSERT_TRUE(native);
    enum class Case : uint32_t { NativeCollision,
                                 DuplicateEntry,
                                 MissingGroup,
                                 ForeignProgram,
                                 UnsortedNativeGroups,
                                 InvalidNativeGroup };
    struct Scenario {
        Case Kind;
        std::string_view Code;
    };
    const Scenario cases[]{
        {Case::NativeCollision, "RendererListGroupCollision"},
        {Case::DuplicateEntry, "RendererListGroupCollision"},
        {Case::MissingGroup, "RendererListMissingGroup"},
        {Case::ForeignProgram, "RendererListProgram"},
        {Case::UnsortedNativeGroups, "RendererListNativeGroup"},
        {Case::InvalidNativeGroup, "RendererListNativeGroup"}};
    for (const Scenario& entry : cases) {
        const Case scenario = entry.Kind;
        const std::string_view code = entry.Code;
        SCOPED_TRACE(code);
        RendererList list;
        MeshDrawCommand draw;
        draw.Program = program.Get();
        switch (scenario) {
            case Case::DuplicateEntry:
                // The graph supplies group 0 twice; the draw itself carries no native group.
                break;
            case Case::MissingGroup: {
                // A valid draw first, so the program requirements are already known when the next draw omits its group.
                MeshDrawCommand valid;
                valid.Program = program.Get();
                valid.Groups.push_back({0, native.Get(), {}});
                list.Commands.push_back(std::move(valid));
                break;
            }
            case Case::UnsortedNativeGroups:
                draw.Groups.push_back({1, native.Get(), {}});
                draw.Groups.push_back({0, native.Get(), {}});
                break;
            case Case::InvalidNativeGroup:
                draw.Groups.push_back({0, nullptr, {}});
                break;
            default:
                draw.Groups.push_back({0, native.Get(), {}});
                break;
        }
        list.Commands.push_back(std::move(draw));
        auto graph = MakeGraph("invalid pass sets");
        const auto color = MakeSmallTarget(graph, "color");
        bool recorded = false;
        struct Data {
            const RendererList* List;
            ShaderProgram* Program;
            ShaderProgram* Other;
            Case Scenario;
            bool* Recorded;
            array<float, 4> Values;
            std::optional<RendererListPassSets> Sets;
        };
        graph.AddRasterPass<Data>("reject", [&](Data& data, RenderGraphRasterBuilder& builder) {
            data.List = &list;
            data.Program = program.Get();
            data.Other = other.Get();
            data.Scenario = scenario;
            data.Recorded = &recorded;
            data.Values = {1, 2, 3, 4};
            builder.SetColorAttachment(0, color);
            builder.SetSideEffect(); }, +[](Data& data, RenderGraphPrepareContext& ctx) {
            const RgParameterBinding parameter{"ValuesBuffer", 0, RgCBufferParameterBinding{std::as_bytes(std::span{data.Values})}};
            vector<RendererListProgramParameters> parameters;
            switch (data.Scenario) {
                case Case::NativeCollision: parameters.push_back({data.Program, 0, std::span{&parameter, 1}}); break;
                case Case::DuplicateEntry:
                    parameters.push_back({data.Program, 0, std::span{&parameter, 1}});
                    parameters.push_back({data.Program, 0, std::span{&parameter, 1}});
                    break;
                case Case::ForeignProgram: parameters.push_back({data.Other, 0, std::span{&parameter, 1}}); break;
                default: break;
            }
            data.Sets = RendererListPassSets::Create(ctx, *data.List, parameters);
            return data.Sets.has_value(); }, +[](const Data& data, RenderGraphRasterContext&) { *data.Recorded = true; });
        EXPECT_FALSE(Run(graph));
        EXPECT_FALSE(recorded);
        EXPECT_EQ(graph.GetFirstErrorCode(), code);
        ASSERT_FALSE(graph.GetReport().Diagnostics.empty());
        EXPECT_EQ(graph.GetReport().Diagnostics.front().Code, code);
        EXPECT_FALSE(graph.GetReport().Diagnostics.front().Pass.empty());
        EXPECT_FALSE(graph.GetReport().Diagnostics.front().Binding.empty());
    }
}

TEST_P(RendererListPassSetsTest, InvalidItemIndexDuplicateAndIncompleteOrderFailDuringPreparation) {
    for (uint32_t scenario = 0; scenario < 3; ++scenario) {
        SCOPED_TRACE(scenario);
        RendererList list;
        list.Commands.resize(2);
        list.Items = {{{}, 0}, {{}, scenario == 0 ? 2u : 0u}};
        if (scenario == 2) list.Items.pop_back();
        auto graph = MakeGraph("invalid draw order");
        const auto color = MakeSmallTarget(graph, "color");
        struct Data {
            const RendererList* List;
            std::optional<PreparedRendererList> Prepared;
        };
        graph.AddRasterPass<Data>("reject order", [&](Data& data, RenderGraphRasterBuilder& builder) {
            data.List = &list;
            builder.SetColorAttachment(0, color);
            builder.SetSideEffect(); }, +[](Data& data, RenderGraphPrepareContext& ctx) {
            data.Prepared = PrepareRendererList(*data.List, ctx);
            return data.Prepared.has_value(); }, +[](const Data&, RenderGraphRasterContext&) {
            ADD_FAILURE() << "An invalid draw order reached recording";
        });
        EXPECT_FALSE(Run(graph));
        EXPECT_EQ(graph.GetReport().GraphicsPipelinePreparations, 0u);
        EXPECT_EQ(graph.GetFirstErrorCode(), "RendererListPreparation");
        ASSERT_FALSE(graph.GetReport().Diagnostics.empty());
        EXPECT_EQ(graph.GetReport().Diagnostics.front().Code, "RendererListPreparation");
    }
}

TEST_P(RendererListPassSetsTest, ProgramRecipesOwnResolvedGroupsAcrossLayoutChanges) {
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

TEST_P(RendererListPassSetsTest, ExecutionItemsPreserveDynamicBindingsOrderAndRejectForeignGraph) {
    auto& device = *Context.Device;
    render::ShaderProgramLayoutRecipe recipe;
    const render::ShaderLayoutSelector selector{.DeclarationName = "Color", .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
    recipe.D3D12.BufferPlacements.push_back({.Selector = selector, .Placement = render::D3D12BufferPlacement::RootDescriptor});
    recipe.Vulkan.BufferDescriptors.push_back({.Selector = selector, .Placement = render::VulkanBufferDescriptorPlacement::Dynamic});
    auto program = test::CompileFoundationGraphics(device, R"hlsl(
#include <core/platform.hlsli>
struct Values { float4 Value; };
VK_BINDING(0, 0) ConstantBuffer<Values> Color : register(b0);
VK_BINDING(0, 1) ConstantBuffer<Values> Factor : register(b0, space1);
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position { return float4(p, 1); }
[shader("pixel")] float4 PSMain() : SV_Target0 { return Color.Value * Factor.Value; }
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
    const array<float, 4> factorValues{.5f, .25f, .75f, 1.f};
    auto factorBuffer = render::test::MakeUploadBuffer(device, std::as_bytes(std::span{factorValues}), render::BufferUse::CBuffer);
    ASSERT_TRUE(factorBuffer);
    auto factorSet = device.CreateShaderParameterSet({program->GetPipelineLayout(), 1});
    ASSERT_TRUE(factorSet);
    ASSERT_TRUE(factorSet->Set(program->GetPipelineLayout()->FindBinding("Factor"), 0, render::ShaderBufferBinding{factorBuffer.Get(), {0, 16}, 0}));
    ASSERT_TRUE(factorSet->FlushWrites());
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
        draw.Groups.push_back({1, factorSet.Get(), {}});
        if (index == 2) draw.PipelineState.Primitive.FaceClockwise = render::FrontFace::CW;
        ASSERT_TRUE(FinalizeMeshDrawCommand(draw));
        list.Commands.push_back(std::move(draw));
    }
    list.Items = {{{}, 2}, {{}, 0}, {{}, 1}};
    auto graph = MakeGraph("ordered execution");
    const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource, {}}, "color");
    DrawExecutionStats stats;
    std::optional<PreparedRendererList> prepared;
    struct Data {
        const RendererList* Source;
        std::optional<PreparedRendererList>* Prepared;
        DrawExecutionStats* Stats;
        render::RenderBackend Backend;
    };
    graph.AddRasterPass<Data>("blue red green", [&](Data& data, RenderGraphRasterBuilder& builder) {
        builder.SetColorAttachment(0, color);
        data = {&list, &prepared, &stats, GetParam()};
    }, +[](Data& data, RenderGraphPrepareContext& ctx) {
        *data.Prepared = PrepareRendererList(*data.Source, ctx);
        if (!*data.Prepared) return false;
        const auto& draws = (*data.Prepared)->Draws;
        EXPECT_EQ(draws.size(), 3u);
        for (size_t index = 0; index < draws.size(); ++index) {
            EXPECT_EQ(draws[index].Description, &data.Source->GetCommand(index));
            EXPECT_EQ(draws[index].Groups.data(), data.Source->GetCommand(index).Groups.data());
            EXPECT_TRUE(draws[index].PassGroups.empty());
            EXPECT_NE(draws[index].Pipeline, nullptr);
        }
        return true;
    }, +[](const Data& data, RenderGraphRasterContext& context) {
        context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 16, 16));
        context.Encoder().SetScissor({0, 0, 16, 16});
        RecordRendererList(**data.Prepared, context, *data.Stats);
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
    EXPECT_NEAR(std::to_integer<uint8_t>(bytes[pixel + 1]), 64u, 1);
    EXPECT_EQ(std::to_integer<uint8_t>(bytes[pixel + 2]), 0u);
    // A prepared list belongs to exactly one pass of one graph; recording it elsewhere binds nothing.
    auto foreign = MakeGraph("foreign execution");
    const auto foreignColor = foreign.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "foreign color");
    DrawExecutionStats rejected;
    foreign.AddRasterPass<Data>("reject previous graph", [&](Data& data, RenderGraphRasterBuilder& builder) {
        data = {&list, &prepared, &rejected, GetParam()};
        builder.SetColorAttachment(0, foreignColor);
        builder.SetSideEffect();
    }, +[](const Data& data, RenderGraphRasterContext& context) {
        RecordRendererList(**data.Prepared, context, *data.Stats);
    });
    EXPECT_FALSE(Run(foreign));
    EXPECT_EQ(rejected.Draws, 0u);
    EXPECT_EQ(rejected.BindingFailure, 3u);
}

TEST_P(RendererListPassSetsTest, B03TextureArraysRejectHolesAndKindsAndReadBothElements) {
    auto& device = *Context.Device;
    constexpr std::string_view source = R"hlsl(
#include <core/platform.hlsli>
VK_BINDING(0, 0) Texture2D<float> Images[2] : register(t0);
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position { return float4(p, 1); }
[shader("pixel")] float PSMain() : SV_Target0 { return Images[0].Load(int3(0, 0, 0)) + Images[1].Load(int3(0, 0, 0)); }
)hlsl";
    auto program = test::CompileFoundationGraphics(device, source);
    ASSERT_TRUE(program);
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
    // 0: array hole, 1: no bindings at all, 2: wrong binding kind for an element, 3: both elements read.
    for (uint32_t scenario = 0; scenario < 4; ++scenario) {
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
            ShaderProgram* Program;
            uint32_t Scenario;
            array<RgTextureViewHandle, 2> Images;
            array<uint32_t, 4> Wrong;
            std::optional<RendererListPassSets> Sets;
            std::optional<PreparedRendererList> Prepared;
        };
        graph.AddRasterPass<Data>("array consumer", [&](Data& data, RenderGraphRasterBuilder& builder) {
            data.List = &list;
            data.Stats = &stats;
            data.Backend = GetParam();
            data.Program = program.Get();
            data.Scenario = scenario;
            data.Wrong = {1, 2, 3, 4};
            builder.SetColorAttachment(0, images[2]);
            data.Images[0] = builder.ReadTexture(images[0]);
            data.Images[1] = builder.ReadTexture(images[1]); }, +[](Data& data, RenderGraphPrepareContext& ctx) {
            vector<RgParameterBinding> bindings{{"Images", 0, RgTextureParameterBinding{data.Images[0]}}};
            if (data.Scenario != 0) bindings.push_back({"Images", 1, RgTextureParameterBinding{data.Images[1]}});
            if (data.Scenario == 1) bindings.clear();
            if (data.Scenario == 2) bindings[1].Value = RgCBufferParameterBinding{std::as_bytes(std::span{data.Wrong})};
            const RendererListProgramParameters parameters{data.Program, 0, bindings};
            data.Sets = RendererListPassSets::Create(ctx, *data.List, std::span{&parameters, 1});
            if (!data.Sets) return false;
            data.Prepared = PrepareRendererList(*data.List, ctx, &*data.Sets);
            return data.Prepared.has_value(); }, +[](const Data& data, RenderGraphRasterContext& context) {
            context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, 16, 16));
            context.Encoder().SetScissor({0, 0, 16, 16});
            RecordRendererList(*data.Prepared, context, *data.Stats); });
        const auto pitch = Align(uint64_t{16 * 4}, device.GetDetail().TextureDataPitchAlignment);
        auto readback = device.CreateBuffer({pitch * 16, render::MemoryType::ReadBack, render::BufferUse::MapRead | render::BufferUse::CopyDestination, {}});
        ASSERT_TRUE(readback);
        RenderExternalBuffer external{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
        const auto host = graph.NextVersion(graph.ImportBuffer(external, "readback", RenderGraphExternalAccess::ObservableOutput));
        graph.AddCopyTextureToBufferPass("copy", images[2], host);
        HostRead(graph, host);
        if (scenario < 3) {
            EXPECT_FALSE(Run(graph));
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

TEST_P(RendererListPassSetsTest, OffSkipsMissingGroupButStillRejectsForeignProgramAndFullMinimalFails) {
    auto& device = *Context.Device;
    auto program = test::CompileFoundationGraphics(device, kSingleGroupProgram);
    auto other = test::CompileFoundationGraphics(device, kSingleGroupProgram);
    ASSERT_TRUE(program);
    ASSERT_TRUE(other);
    auto native = device.CreateShaderParameterSet({program->GetPipelineLayout(), 0});
    ASSERT_TRUE(native);
    const auto makeList = [&] {
        RendererList list;
        MeshDrawCommand valid;
        valid.Program = program.Get();
        valid.Groups.push_back({0, native.Get(), {}});
        list.Commands.push_back(std::move(valid));
        MeshDrawCommand missing;
        missing.Program = program.Get();
        list.Commands.push_back(std::move(missing));
        return list;
    };
    struct Data {
        const RendererList* List;
        ShaderProgram* Program;
        ShaderProgram* Foreign;
        array<float, 4> Values;
        std::optional<RendererListPassSets> Sets;
    };
    const auto prepare = +[](Data& data, RenderGraphPrepareContext& ctx) {
        const RgParameterBinding parameter{"ValuesBuffer", 0, RgCBufferParameterBinding{std::as_bytes(std::span{data.Values})}};
        const RendererListProgramParameters foreign{data.Foreign, 0, std::span{&parameter, 1}};
        std::span<const RendererListProgramParameters> parameters{};
        if (data.Foreign) parameters = std::span{&foreign, 1};
        data.Sets = RendererListPassSets::Create(ctx, *data.List, parameters);
        if (!data.Sets) return false;
        // Only the no-parameter graphs get this far, so this pass contributes no graph sets at all.
        EXPECT_TRUE(data.Sets->Find(*data.Program).empty());
        return true;
    };
    {
        // Validation off keeps the per-draw scans out of the frame: a draw may omit a required group.
        auto graph = RenderGraph{*Context.Device, *Resources, *Registry, "off", kPerformanceRenderGraphRuntimeOptions};
        auto list = makeList();
        const auto color = MakeSmallTarget(graph, "color");
        graph.AddRasterPass<Data>("skip", [&](Data& data, RenderGraphRasterBuilder& builder) {
            data = {&list, program.Get(), nullptr, {1, 2, 3, 4}, std::nullopt};
            EXPECT_FALSE(builder.IsValidationFull());
            builder.SetColorAttachment(0, color);
            builder.SetSideEffect(); }, prepare, +[](const Data&, RenderGraphRasterContext&) {});
        EXPECT_TRUE(Run(graph)) << graph.GetFirstErrorCode();
        EXPECT_FALSE(graph.HasFailed());
        EXPECT_TRUE(graph.GetFirstErrorCode().empty());
    }
    {
        // The argument checks are cheap and stay on: a program no draw uses is always a mistake.
        auto graph = RenderGraph{*Context.Device, *Resources, *Registry, "off foreign program", kPerformanceRenderGraphRuntimeOptions};
        auto list = makeList();
        const auto color = MakeSmallTarget(graph, "color");
        graph.AddRasterPass<Data>("reject foreign", [&](Data& data, RenderGraphRasterBuilder& builder) {
            data = {&list, program.Get(), other.Get(), {1, 2, 3, 4}, std::nullopt};
            builder.SetColorAttachment(0, color);
            builder.SetSideEffect(); }, prepare, +[](const Data&, RenderGraphRasterContext&) { ADD_FAILURE() << "A foreign parameter program reached recording"; });
        EXPECT_FALSE(Run(graph));
        EXPECT_TRUE(graph.HasFailed());
        EXPECT_EQ(graph.GetFirstErrorCode(), "RendererListProgram");
    }
    {
        RenderGraphRuntimeOptions options = kDiagnosticRenderGraphRuntimeOptions;
        options.Report = RenderGraphReportMode::Minimal;
        auto graph = RenderGraph{*Context.Device, *Resources, *Registry, "full-minimal", options};
        auto list = makeList();
        const auto color = MakeSmallTarget(graph, "color");
        graph.AddRasterPass<Data>("reject", [&](Data& data, RenderGraphRasterBuilder& builder) {
            data = {&list, program.Get(), nullptr, {1, 2, 3, 4}, std::nullopt};
            EXPECT_TRUE(builder.IsValidationFull());
            builder.SetColorAttachment(0, color);
            builder.SetSideEffect(); }, prepare, +[](const Data&, RenderGraphRasterContext&) { ADD_FAILURE() << "A draw missing its group reached recording"; });
        EXPECT_FALSE(Run(graph));
        EXPECT_TRUE(graph.HasFailed());
        EXPECT_EQ(graph.GetFirstErrorCode(), "RendererListMissingGroup");
        EXPECT_TRUE(graph.GetReport().Diagnostics.empty());
        EXPECT_EQ(graph.GetReport().FirstErrorCode, "RendererListMissingGroup");
    }
}

// Pass sets hold views resolved against the declaring pass's accesses, so borrowing them in another
// pass would record descriptors the graph planned no barrier for.
TEST_P(RendererListPassSetsTest, PassSetsFromAnotherPassAreRejected) {
    auto& device = *Context.Device;
    auto program = test::CompileFoundationGraphics(device, kSingleGroupProgram);
    ASSERT_TRUE(program);
    RendererList list;
    MeshDrawCommand draw;
    // The graph set covers the program's only group, so the draw declares no native group.
    draw.Program = program.Get();
    list.Commands.push_back(std::move(draw));
    struct Owner {
        const RendererList* List;
        ShaderProgram* Program;
        array<float, 4> Values;
        std::optional<RendererListPassSets>* Shared;
    };
    struct Borrower {
        const RendererList* List;
        std::optional<RendererListPassSets>* Shared;
    };
    for (const auto& options : {kDiagnosticRenderGraphRuntimeOptions, kPerformanceRenderGraphRuntimeOptions}) {
        SCOPED_TRACE(options.Validation == RenderValidationMode::Full ? "full" : "off");
        auto graph = RenderGraph{device, *Resources, *Registry, "cross pass sets", options};
        const auto owned = MakeSmallTarget(graph, "owned");
        const auto borrowed = MakeSmallTarget(graph, "borrowed");
        std::optional<RendererListPassSets> shared;
        graph.AddRasterPass<Owner>("owner", [&](Owner& data, RenderGraphRasterBuilder& builder) {
            data = {&list, program.Get(), {1, 2, 3, 4}, &shared};
            builder.SetColorAttachment(0, owned);
            builder.SetSideEffect(); }, +[](Owner& data, RenderGraphPrepareContext& ctx) {
            const RgParameterBinding parameter{"ValuesBuffer", 0, RgCBufferParameterBinding{std::as_bytes(std::span{data.Values})}};
            const RendererListProgramParameters parameters{data.Program, 0, std::span{&parameter, 1}};
            *data.Shared = RendererListPassSets::Create(ctx, *data.List, std::span{&parameters, 1});
            return data.Shared->has_value(); }, +[](const Owner&, RenderGraphRasterContext&) {});
        graph.AddRasterPass<Borrower>("borrower", [&](Borrower& data, RenderGraphRasterBuilder& builder) {
            data = {&list, &shared};
            builder.SetColorAttachment(0, borrowed);
            builder.SetSideEffect(); }, +[](Borrower& data, RenderGraphPrepareContext& ctx) {
            if (!*data.Shared) return false;
            return PrepareRendererList(*data.List, ctx, &**data.Shared).has_value(); },
            +[](const Borrower&, RenderGraphRasterContext&) { ADD_FAILURE() << "Sets from another pass reached recording"; });
        EXPECT_FALSE(Run(graph));
        EXPECT_TRUE(graph.HasFailed());
        EXPECT_EQ(graph.GetFirstErrorCode(), "RendererListPassSets");
    }
}

INSTANTIATE_TEST_SUITE_P(Backends, RendererListPassSetsTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
