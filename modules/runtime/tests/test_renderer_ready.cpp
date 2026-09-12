#include "foundation_graph_fixture.h"

#include <map>
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/renderer_list.h>
#include <radray/runtime/render_framework/renderer_list_pass_sets.h>

namespace radray {
namespace {

struct ReadyBoundGroup {
    uint32_t Group;
    render::ShaderParameterSet* Set;
    vector<render::ShaderParameterDynamicOffset> Offsets;
    bool operator==(const ReadyBoundGroup&) const = default;
};
struct ReadyBoundVertex {
    uint32_t Binding;
    render::Buffer* Buffer;
    uint64_t Offset, Size;
    bool operator==(const ReadyBoundVertex&) const = default;
};
struct ReadyRecordedDraw {
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
    vector<ReadyBoundGroup> Groups;
    vector<ReadyBoundVertex> Vertices;
    Nullable<render::Buffer*> IndexBuffer{nullptr};
    uint32_t IndexOffset{}, IndexStride{};
    array<int64_t, 5> Arguments{};
    bool operator==(const ReadyRecordedDraw&) const = default;
};

class ReadyTraceEncoder final : public render::GraphicsCommandEncoder {
public:
    explicit ReadyTraceEncoder(render::CommandBuffer& command) : _command(command) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    render::CommandBuffer* GetCommandBuffer() const noexcept override { return &_command; }
    void BindGraphicsPipelineState(render::GraphicsPipelineState* pipeline) noexcept override {
        if (_state.Pipeline.Get() != pipeline) _groups.clear();
        _state.Pipeline = pipeline;
        ++PipelineCalls;
    }
    void BindShaderParameterSet(uint32_t group, render::ShaderParameterSet* set,
                                std::span<const render::ShaderParameterDynamicOffset> offsets) noexcept override {
        _groups.insert_or_assign(group, ReadyBoundGroup{group, set, {offsets.begin(), offsets.end()}});
        BoundGroups.push_back(group);
    }
    void BindVertexBuffers(std::span<const render::VertexBufferBinding> bindings) noexcept override {
        auto& call = VertexCalls.emplace_back();
        for (const auto& binding : bindings) {
            call.push_back(binding.Binding);
            _vertices.insert_or_assign(binding.Binding, ReadyBoundVertex{binding.Binding, binding.View.Target, binding.View.Offset, binding.View.Size});
        }
    }
    void BindIndexBuffer(render::IndexBufferView binding) noexcept override {
        _state.IndexBuffer = binding.Target;
        _state.IndexOffset = binding.Offset;
        _state.IndexStride = binding.Stride;
        ++IndexCalls;
    }
    void DrawIndexed(uint32_t count, uint32_t instances, uint32_t first, int32_t vertexOffset, uint32_t firstInstance) noexcept override {
        _state.Arguments = {count, instances, first, vertexOffset, firstInstance};
        _state.Groups.clear();
        _state.Vertices.clear();
        for (const auto& [key, group] : _groups) _state.Groups.push_back(group);
        for (const auto& [key, vertex] : _vertices) _state.Vertices.push_back(vertex);
        Draws.push_back(_state);
    }
    void SetViewport(Viewport) noexcept override {}
    void SetScissor(Rect) noexcept override {}
    bool SetPushConstants(render::BindingHandle, std::span<const byte>) noexcept override {
        UnexpectedCommand = true;
        return false;
    }
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) noexcept override { UnexpectedCommand = true; }
    void DrawIndirect(render::Buffer*, uint64_t, uint32_t) noexcept override { UnexpectedCommand = true; }
    void DrawIndexedIndirect(render::Buffer*, uint64_t, uint32_t) noexcept override { UnexpectedCommand = true; }
    uint32_t PipelineCalls{}, IndexCalls{};
    vector<uint32_t> BoundGroups;
    vector<vector<uint32_t>> VertexCalls;
    vector<ReadyRecordedDraw> Draws;
    bool UnexpectedCommand{false};

private:
    render::CommandBuffer& _command;
    ReadyRecordedDraw _state;
    map<uint32_t, ReadyBoundGroup> _groups;
    map<uint32_t, ReadyBoundVertex> _vertices;
};

class ReadySceneFixture {
public:
    explicit ReadySceneFixture(render::Device& device) : Groups(&device), _device(device) {}
    bool Initialize(HostWriteBatch& writes) {
        render::ShaderProgramLayoutRecipe recipe;
        for (const auto name : {"SharedView", "ReadyObject", "SharedMaterial"}) {
            const render::ShaderLayoutSelector selector{.DeclarationName = name, .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
            recipe.D3D12.BufferPlacements.push_back({selector, render::D3D12BufferPlacement::RootDescriptor});
            recipe.Vulkan.BufferDescriptors.push_back({selector, render::VulkanBufferDescriptorPlacement::Dynamic});
        }
        auto program = test::CompileFoundationGraphics(_device, R"hlsl(
#include <core/platform.hlsli>
struct Values { float4 Value; };
VK_BINDING(0, 1) ConstantBuffer<Values> SharedView : register(b0, space1);
VK_BINDING(0, 3) ConstantBuffer<Values> ReadyObject : register(b0, space3);
VK_BINDING(0, 5) ConstantBuffer<Values> SharedMaterial : register(b0, space5);
struct VertexOutput { float4 Position : SV_Position; float4 Color : COLOR0; float2 Uv : TEXCOORD0; };
[shader("vertex")] VertexOutput VSMain(float3 position : POSITION, float4 color : COLOR0, float2 uv : TEXCOORD0) {
    VertexOutput result;
    result.Position = float4(position + ReadyObject.Value.xyz, 1);
    result.Color = color;
    result.Uv = uv;
    return result;
}
[shader("pixel")] float4 PSMain(VertexOutput input) : SV_Target0 {
    return input.Color * SharedView.Value * SharedMaterial.Value + float4(input.Uv, 0, 0) * .001;
}
)hlsl",
                                                       recipe);
        if (!program) return false;
        Program = program.Release();
        const array<float, 9> positions{-1, -1, .5f, 3, -1, .5f, -1, 3, .5f};
        const array<float, 12> colors{1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
        const array<float, 6> uvs{0, 0, 1, 0, 0, 1};
        const array<uint32_t, 10> indices{0, 0, 0, 1, 2, 3, 1, 2, 3, 0};
        auto position = render::test::MakeUploadBuffer(_device, std::as_bytes(std::span{positions}), render::BufferUse::Vertex);
        auto color = render::test::MakeUploadBuffer(_device, std::as_bytes(std::span{colors}), render::BufferUse::Vertex);
        auto uv = render::test::MakeUploadBuffer(_device, std::as_bytes(std::span{uvs}), render::BufferUse::Vertex);
        auto index = render::test::MakeUploadBuffer(_device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
        if (!position || !color || !uv || !index || !Groups.BeginFrame(writes)) return false;
        Position = position.Release();
        Color = color.Release();
        Uv = uv.Release();
        Index = index.Release();
        Geometry.VertexBuffers = {{3, {Position.get(), 0, sizeof(positions)}}, {4, {Color.get(), 0, sizeof(colors)}}, {7, {Uv.get(), 0, sizeof(uvs)}}};
        Geometry.Ibv = {Index.get(), sizeof(uint32_t), sizeof(uint32_t)};
        Geometry.VertexLayout.Buffers = {{3, 12, render::VertexStepMode::Vertex}, {4, 16, render::VertexStepMode::Vertex}, {7, 8, render::VertexStepMode::Vertex}};
        Geometry.VertexLayout.Attributes = {{"POSITION", 0, 3, 0, render::VertexFormat::FLOAT32X3}, {"COLOR", 0, 4, 0, render::VertexFormat::FLOAT32X4}, {"TEXCOORD", 0, 7, 0, render::VertexFormat::FLOAT32X2}};
        const array<float, 4> view{1, 1, 1, 1}, material{.5f, .5f, .5f, 1}, a{}, b{.25f, 0, 0, 0};
        View = Groups.PrepareGroupId(*Program, 1, Identity(1), 0, std::as_bytes(std::span{view}));
        Material = Groups.PrepareGroupId(*Program, 5, Identity(5), 0, std::as_bytes(std::span{material}));
        ObjectA = Groups.PrepareGroupId(*Program, 3, Identity(3), 0, std::as_bytes(std::span{a}));
        ObjectB = Groups.PrepareGroupId(*Program, 3, Identity(3), 1, std::as_bytes(std::span{b}));
        if (!View.IsValid() || !Material.IsValid() || !ObjectA.IsValid() || !ObjectB.IsValid()) return false;
        const array<FrameShaderGroupId, 3> aGroups{View, ObjectA, Material}, bGroups{View, ObjectB, Material};
        const auto aBinding = Groups.InternBinding(aGroups), bBinding = Groups.InternBinding(bGroups);
        if (!aBinding.IsValid() || !bBinding.IsValid()) return false;
        Scene.Valid = Scene.HasPassPolicies = true;
        Scene.PublicationRevision = 1;
        Scene.GeometryBindingPlans.emplace_back().Runs = {{0, 2}, {2, 1}};
        for (uint32_t i = 0; i < 3; ++i) {
            auto& record = Scene.DrawRecords.emplace_back();
            record.Id = i;
            record.Status = DrawRecordStatus::Ready;
            record.NormalStateId = 1;
            record.MirroredStateId = 2;
            record.GeometryBindingPlan = 0;
            record.Description.Program = Program.get();
            record.Description.Geometry = &Geometry;
            record.Description.IndexCount = 3;
            record.Description.FirstIndex = 2;
            record.Description.VertexOffset = -1;
            record.Description.PipelineState.Primitive.Cull = render::CullMode::None;
            record.Description.PipelineState.DepthStencil.DepthTestEnable = false;
            record.Description.PipelineState.DepthStencil.DepthWriteEnable = false;
        }
        for (uint32_t i = 0; i < 3; ++i)
            if (!List.AppendStatic(Scene, i, Groups, i == 1 ? bBinding : aBinding)) return false;
        return true;
    }
    bool GrowGroupTable() {
        const auto oldBase = reinterpret_cast<uintptr_t>(&Groups.GetGroup(View));
        for (uint32_t row = 2; row < 4096; ++row) {
            const array<float, 4> value{float(row), 0, 0, 0};
            const auto id = Groups.PrepareGroupId(*Program, 3, Identity(3), row, std::as_bytes(std::span{value}));
            if (!id.IsValid()) return false;
            if (reinterpret_cast<uintptr_t>(&Groups.GetGroup(View)) != oldBase) return true;
        }
        return false;
    }
    void CheckTrace(const ReadyTraceEncoder& trace, size_t first = 0) const {
        ASSERT_GE(trace.Draws.size(), first + 3);
        for (size_t i = 0; i < 3; ++i) {
            const auto& draw = trace.Draws[first + i];
            EXPECT_TRUE(draw.Pipeline);
            ASSERT_EQ(draw.Groups.size(), 3u);
            const array<FrameShaderGroupId, 3> expected{View, i == 1 ? ObjectB : ObjectA, Material};
            for (size_t group = 0; group < expected.size(); ++group) {
                const auto& value = Groups.GetGroup(expected[group]);
                EXPECT_EQ(draw.Groups[group].Group, value.Group);
                EXPECT_EQ(draw.Groups[group].Set, value.Set.Get());
                EXPECT_EQ(draw.Groups[group].Offsets, (vector<render::ShaderParameterDynamicOffset>{value.DynamicOffsets.begin(), value.DynamicOffsets.end()}));
            }
            ASSERT_EQ(draw.Vertices.size(), 3u);
            for (size_t vertex = 0; vertex < Geometry.VertexBuffers.size(); ++vertex) {
                const auto& expectedVertex = Geometry.VertexBuffers[vertex];
                EXPECT_EQ(draw.Vertices[vertex], (ReadyBoundVertex{expectedVertex.Binding, expectedVertex.View.Target, expectedVertex.View.Offset, expectedVertex.View.Size}));
            }
            EXPECT_EQ(draw.IndexBuffer.Get(), Index.get());
            EXPECT_EQ(draw.IndexOffset, sizeof(uint32_t));
            EXPECT_EQ(draw.IndexStride, sizeof(uint32_t));
            EXPECT_EQ(draw.Arguments, (array<int64_t, 5>{3, 1, 2, -1, 0}));
        }
        EXPECT_FALSE(trace.UnexpectedCommand);
    }
    unique_ptr<ShaderProgram> Program;
    unique_ptr<render::Buffer> Position, Color, Uv, Index;
    GpuMesh::DrawData Geometry;
    RenderSceneSnapshot Scene;
    FrameDrawResources Groups;
    RendererList List;
    FrameShaderGroupId View, Material, ObjectA, ObjectB;

private:
    FrameCBufferIdentity Identity(uint64_t context) const { return {.Source = this, .WireType = &_wire, .Context = context}; }
    inline static const byte _wire{};
    render::Device& _device;
};

class RendererReadyTest : public test::FoundationGraphGpuTest {};

TEST_P(RendererReadyTest, AbaOffsetsAndVertexBindingRunsPreserveIndexedArguments) {
    ReadySceneFixture fixture{*Context.Device};
    ASSERT_TRUE(fixture.Initialize(Writes));
    ASSERT_TRUE(fixture.List.Commands.empty());
    ASSERT_EQ(fixture.List.GetDrawCount(), 3u);
    ASSERT_EQ(fixture.Groups.GetGroup(fixture.ObjectA).Set, fixture.Groups.GetGroup(fixture.ObjectB).Set);
    ASSERT_NE(fixture.Groups.GetGroup(fixture.ObjectA).DynamicOffsets, fixture.Groups.GetGroup(fixture.ObjectB).DynamicOffsets);
    auto graph = MakeGraph("Ready indexed state");
    const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "color");
    struct Payload {
        ReadySceneFixture* Fixture;
        std::optional<PreparedRendererList> Ready;
    };
    graph.AddRasterPass<Payload>("A B A offsets", [&](Payload& data, RenderGraphRasterBuilder& builder) {
        data.Fixture = &fixture; builder.SetColorAttachment(0, color); builder.SetSideEffect(); }, +[](Payload& data, RenderGraphPrepareContext& context) {
        data.Ready = PrepareRendererList(data.Fixture->List, context); return data.Ready.has_value(); }, +[](const Payload& data, RenderGraphRasterContext& context) {
        ReadyTraceEncoder trace{*RenderGraphTestDriver::NativeEncoder(context).GetCommandBuffer()};
        DrawExecutionStats stats;
        RenderGraphTestDriver::WithEncoder(context, trace, [&](RenderGraphRasterContext& traced) { RecordRendererList(*data.Ready, traced, stats); });
        EXPECT_TRUE(stats.Succeeded()); EXPECT_EQ(stats.Draws, 3u);
        EXPECT_EQ(trace.PipelineCalls, 1u); EXPECT_EQ(trace.IndexCalls, 1u);
        EXPECT_EQ(trace.BoundGroups, (vector<uint32_t>{1, 3, 5, 3, 3}));
        EXPECT_EQ(trace.VertexCalls, (vector<vector<uint32_t>>{{3, 4}, {7}}));
        data.Fixture->CheckTrace(trace); });
    ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
    EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexed, 3u);
    EXPECT_EQ(graph.GetReport().CommandCalls.VertexBuffer, 2u);
    EXPECT_EQ(graph.GetReport().CommandCalls.SetParameters, 5u);
}

TEST_P(RendererReadyTest, EveryRecordRebindsItsFirstDrawAfterExternalStateChanges) {
    ReadySceneFixture fixture{*Context.Device};
    ASSERT_TRUE(fixture.Initialize(Writes));
    auto graph = MakeGraph("Ready external state boundary");
    const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "color");
    struct Payload {
        ReadySceneFixture* Fixture;
        std::optional<PreparedRendererList> Ready;
    };
    graph.AddRasterPass<Payload>("two recorder calls", [&](Payload& data, RenderGraphRasterBuilder& builder) {
        data.Fixture = &fixture; builder.SetColorAttachment(0, color); builder.SetSideEffect(); }, +[](Payload& data, RenderGraphPrepareContext& context) {
        data.Ready = PrepareRendererList(data.Fixture->List, context); return data.Ready.has_value(); }, +[](const Payload& data, RenderGraphRasterContext& context) {
        ReadyTraceEncoder trace{*RenderGraphTestDriver::NativeEncoder(context).GetCommandBuffer()};
        DrawExecutionStats stats;
        const auto record = [&] { RenderGraphTestDriver::WithEncoder(context, trace, [&](RenderGraphRasterContext& traced) { RecordRendererList(*data.Ready, traced, stats); }); };
        record();
        data.Fixture->CheckTrace(trace);
        const auto& other = data.Fixture->Groups.GetGroup(data.Fixture->ObjectB);
        trace.BindShaderParameterSet(other.Group, other.Set.Get(), other.DynamicOffsets);
        const render::VertexBufferBinding changed{3, {data.Fixture->Position.get(), 12, 24}};
        trace.BindVertexBuffers(std::span{&changed, 1});
        trace.BindIndexBuffer({data.Fixture->Index.get(), 8, 4});
        const auto pipelineCalls = trace.PipelineCalls, indexCalls = trace.IndexCalls;
        const auto groupCalls = trace.BoundGroups.size(), vertexCalls = trace.VertexCalls.size();
        record();
        EXPECT_TRUE(stats.Succeeded()); EXPECT_EQ(stats.Draws, 6u);
        EXPECT_EQ(trace.PipelineCalls - pipelineCalls, 1u); EXPECT_EQ(trace.IndexCalls - indexCalls, 1u);
        EXPECT_EQ(trace.BoundGroups.size() - groupCalls, 5u); EXPECT_EQ(trace.VertexCalls.size() - vertexCalls, 2u);
        data.Fixture->CheckTrace(trace, 3);
        ASSERT_EQ(trace.Draws.size(), 6u);
        for (size_t i = 0; i < 3; ++i) EXPECT_EQ(trace.Draws[i], trace.Draws[i + 3]); });
    ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
    EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexed, 6u);
    EXPECT_EQ(graph.GetReport().CommandCalls.SetPipeline, 2u);
}

TEST_P(RendererReadyTest, SameEpochGroupTableGrowthDoesNotInvalidatePreparedIds) {
    ReadySceneFixture fixture{*Context.Device};
    ASSERT_TRUE(fixture.Initialize(Writes));
    auto graph = MakeGraph("Ready group table growth");
    const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "color");
    struct Payload {
        ReadySceneFixture* Fixture;
        std::optional<PreparedRendererList> Ready;
    };
    graph.AddRasterPass<Payload>("grow after Ready", [&](Payload& data, RenderGraphRasterBuilder& builder) {
        data.Fixture = &fixture; builder.SetColorAttachment(0, color); builder.SetSideEffect(); }, +[](Payload& data, RenderGraphPrepareContext& context) {
        data.Ready = PrepareRendererList(data.Fixture->List, context);
        if (!data.Ready) return false;
        const auto epoch = data.Fixture->Groups.GetEpoch();
        const bool relocated = data.Fixture->GrowGroupTable();
        EXPECT_TRUE(relocated); EXPECT_EQ(data.Fixture->Groups.GetEpoch(), epoch);
        return relocated; }, +[](const Payload& data, RenderGraphRasterContext& context) {
        ReadyTraceEncoder trace{*RenderGraphTestDriver::NativeEncoder(context).GetCommandBuffer()};
        DrawExecutionStats stats;
        RenderGraphTestDriver::WithEncoder(context, trace, [&](RenderGraphRasterContext& traced) { RecordRendererList(*data.Ready, traced, stats); });
        EXPECT_TRUE(stats.Succeeded()); EXPECT_EQ(stats.Draws, 3u);
        data.Fixture->CheckTrace(trace); });
    ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
}

TEST_P(RendererReadyTest, ResourceEpochListResetAndPublicationChangesRejectBeforeBinding) {
    for (uint32_t invalidation = 0; invalidation < 7; ++invalidation) {
        SCOPED_TRACE(invalidation);
        for (const auto options : {kDiagnosticRenderGraphRuntimeOptions, kPerformanceRenderGraphRuntimeOptions}) {
            SCOPED_TRACE(options.Validation == RenderValidationMode::Full ? "full" : "off");
            ReadySceneFixture fixture{*Context.Device};
            ASSERT_TRUE(fixture.Initialize(Writes));
            RenderGraph graph{*Context.Device, *Resources, *Registry, "Ready invalidation", options};
            const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "color");
            struct Payload {
                ReadySceneFixture* Fixture;
                uint32_t Invalidation;
                std::optional<PreparedRendererList> Ready;
                bool* Visited;
            };
            bool visited = false;
            graph.AddRasterPass<Payload>("reject stale Ready", [&](Payload& data, RenderGraphRasterBuilder& builder) {
                data.Fixture = &fixture; data.Invalidation = invalidation; data.Visited = &visited;
                builder.SetColorAttachment(0, color); builder.SetSideEffect(); }, +[](Payload& data, RenderGraphPrepareContext& context) {
                data.Ready = PrepareRendererList(data.Fixture->List, context);
                if (!data.Ready) return false;
                if (data.Invalidation == 0) data.Fixture->Groups.ClearSets();
                else if (data.Invalidation == 1) data.Fixture->List.ResetForReuse();
                else if (data.Invalidation == 2) ++data.Fixture->Scene.PublicationRevision;
                else if (data.Invalidation == 3) ++data.Fixture->Scene.PublicationId;
                else if (data.Invalidation == 4) ++data.Fixture->Scene.SceneEpoch;
                else {
                    RendererList replacement = data.Fixture->List;
                    const auto count = data.Fixture->List.GetDrawCount();
                    if (data.Invalidation == 5) data.Fixture->List = replacement;
                    else data.Fixture->List = std::move(replacement);
                    EXPECT_EQ(data.Fixture->List.GetDrawCount(), count);
                    EXPECT_TRUE(data.Fixture->List.IsCurrent());
                }
                return true; }, +[](const Payload& data, RenderGraphRasterContext& context) {
                *data.Visited = true;
                ReadyTraceEncoder trace{*RenderGraphTestDriver::NativeEncoder(context).GetCommandBuffer()};
                DrawExecutionStats stats;
                RenderGraphTestDriver::WithEncoder(context, trace, [&](RenderGraphRasterContext& traced) { RecordRendererList(*data.Ready, traced, stats); });
                EXPECT_EQ(stats.Draws, 0u); EXPECT_FALSE(stats.Succeeded());
                EXPECT_TRUE(trace.Draws.empty()); EXPECT_TRUE(trace.BoundGroups.empty()); EXPECT_TRUE(trace.VertexCalls.empty());
                EXPECT_EQ(trace.PipelineCalls, 0u); EXPECT_EQ(trace.IndexCalls, 0u); });
            EXPECT_FALSE(Run(graph));
            EXPECT_EQ(visited, options.Validation == RenderValidationMode::Off);
            EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexed, 0u);
            EXPECT_EQ(graph.GetReport().CommandCalls.SetPipeline, 0u);
            EXPECT_EQ(graph.GetReport().CommandCalls.SetParameters, 0u);
        }
    }
}

TEST_P(RendererReadyTest, ReadyValidationRunsAfterAllPreparationAndUsesTheFrameMode) {
    for (const auto options : {kPerformanceRenderGraphRuntimeOptions, kDiagnosticRenderGraphRuntimeOptions, kPerformanceRenderGraphRuntimeOptions}) {
        const bool full = options.Validation == RenderValidationMode::Full;
        SCOPED_TRACE(full ? "full" : "off");
        ReadySceneFixture fixture{*Context.Device};
        ASSERT_TRUE(fixture.Initialize(Writes));
        RenderGraph graph{*Context.Device, *Resources, *Registry, "Ready validation boundary", options};
        const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "color");
        struct Probe {
            vector<uint32_t> Events;
        };
        auto probe = make_shared<Probe>();
        struct Payload {
            ReadySceneFixture* Fixture;
            shared_ptr<Probe> State;
            std::optional<PreparedRendererList> Ready;
        };
        graph.AddRasterPass<Payload>("prepare first", [&](Payload& data, RenderGraphRasterBuilder& builder) {
            data.Fixture = &fixture;
            data.State = probe;
            builder.SetColorAttachment(0, color);
            builder.SetSideEffect(); }, +[](Payload& data, RenderGraphPrepareContext& context) {
            data.State->Events.push_back(1);
            // The diagnostic must own its group metadata after this local set wrapper is gone.
            const auto sets = RendererListPassSets::Create(context, data.Fixture->List, {});
            if (!sets) return false;
            data.Ready = PrepareRendererList(data.Fixture->List, context, &*sets);
            context.DeferReadyValidation(data.State, +[](const void* payload, RenderGraphPrepareContext&) {
                const auto& state = *static_cast<const Probe*>(payload);
                EXPECT_EQ(state.Events, (vector<uint32_t>{1, 2}));
                return state.Events == vector<uint32_t>{1, 2};
            });
            return data.Ready.has_value(); }, +[](const Payload& data, RenderGraphRasterContext& context) {
            data.State->Events.push_back(3);
            ReadyTraceEncoder trace{*RenderGraphTestDriver::NativeEncoder(context).GetCommandBuffer()};
            DrawExecutionStats stats;
            RenderGraphTestDriver::WithEncoder(context, trace, [&](RenderGraphRasterContext& traced) { RecordRendererList(*data.Ready, traced, stats); });
            EXPECT_TRUE(stats.Succeeded());
            EXPECT_EQ(stats.Draws, 3u);
            data.Fixture->CheckTrace(trace); });
        const auto secondColor = graph.NextVersion(color);
        graph.AddRasterPass<shared_ptr<Probe>>("prepare second", [&](shared_ptr<Probe>& data, RenderGraphRasterBuilder& builder) {
            data = probe;
            builder.SetColorAttachment(0, secondColor, {.Load = render::LoadAction::Load});
            builder.SetSideEffect(); }, +[](shared_ptr<Probe>& data, RenderGraphPrepareContext&) {
            data->Events.push_back(2);
            return true; }, +[](const shared_ptr<Probe>&, RenderGraphRasterContext&) {});
        ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
        EXPECT_EQ(probe->Events, (vector<uint32_t>{1, 2, 3}));
        EXPECT_EQ(graph.GetReport().ValidateReadyFrameCalls, full ? 1u : 0u);
        EXPECT_EQ(graph.GetReport().ReadyValidationCallbacks, full ? 3u : 0u);
        EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexed, 3u);
    }
}

TEST_P(RendererReadyTest, InvalidGeometryIsRejectedAtReadyBoundaryBeforeAnyRecording) {
    ReadySceneFixture fixture{*Context.Device};
    ASSERT_TRUE(fixture.Initialize(Writes));
    fixture.Scene.DrawRecords[0].Description.IndexCount = 40;
    auto graph = MakeGraph("invalid Ready geometry");
    const auto color = graph.CreateTexture({render::TextureDimension::Dim2D, 16, 16, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget, {}}, "color");
    uint32_t prepares = 0, records = 0;
    struct Payload {
        ReadySceneFixture* Fixture;
        uint32_t* Prepares;
        uint32_t* Records;
        std::optional<PreparedRendererList> Ready;
    };
    graph.AddRasterPass<Payload>("invalid geometry", [&](Payload& data, RenderGraphRasterBuilder& builder) {
        data.Fixture = &fixture;
        data.Prepares = &prepares;
        data.Records = &records;
        builder.SetColorAttachment(0, color);
        builder.SetSideEffect(); }, +[](Payload& data, RenderGraphPrepareContext& context) {
        ++*data.Prepares;
        data.Ready = PrepareRendererList(data.Fixture->List, context);
        return data.Ready.has_value(); }, +[](const Payload& data, RenderGraphRasterContext&) { ++*data.Records; });
    const auto secondColor = graph.NextVersion(color);
    graph.AddRasterPass<Payload>("other live preparation", [&](Payload& data, RenderGraphRasterBuilder& builder) {
        data.Prepares = &prepares;
        data.Records = &records;
        builder.SetColorAttachment(0, secondColor, {.Load = render::LoadAction::Load});
        builder.SetSideEffect(); }, +[](Payload& data, RenderGraphPrepareContext&) {
        ++*data.Prepares;
        return true; }, +[](const Payload& data, RenderGraphRasterContext&) { ++*data.Records; });
    EXPECT_FALSE(Run(graph));
    EXPECT_EQ(prepares, 2u);
    EXPECT_EQ(records, 0u);
    EXPECT_EQ(graph.GetFirstErrorCode(), "RendererListPreparation");
    EXPECT_EQ(graph.GetReport().ValidateReadyFrameCalls, 1u);
    EXPECT_EQ(graph.GetReport().ReadyValidationCallbacks, 1u);
    EXPECT_EQ(graph.GetReport().CommandCalls.DrawIndexed, 0u);
}

INSTANTIATE_TEST_SUITE_P(Backends, RendererReadyTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
